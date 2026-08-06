#!/bin/bash
# tools/hlsl-diff/diff.sh — differential HLSL test driver.
#
# Compiles one .hlsl file two ways and diffs the GPU output:
#   ours:      binc -E <entry> -T <profile>            -> .metallib   (Phase 1+)
#   reference: dxc -spirv -> spirv-cross --msl -> metal -> .metallib
# Both are dispatched through the binc harness with a generated .spec
# containing a `dump 0` directive; the buffer dumps are compared with a
# relative tolerance (two MSL compilations may differ in the last ULPs).
#
# usage: diff.sh [-E <entry>] [-T <profile>] [-g <grid>] [-w <words>]
#                [-o <outidx>] [--reference-only] file.hlsl
#   -E entry     entry point (default: main)
#   -T profile   HLSL target profile (default: cs_6_0)
#   -g grid      thread count for compute (default: 16)
#   -w words     output buffer size in words (default: 64)
#   -o outidx    output buffer index to dump (default: 0)
#   --reference-only  build+run only the reference side (Phase 0 validation)
set -u
cd "$(dirname "$0")/../.." || exit 1
DEV="${DEV:-/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer}"
ENTRY=main; PROFILE=cs_6_0; GRID=16; WORDS=64; OUTIDX=0; REFONLY=0
while [ $# -gt 1 ]; do
  case "$1" in
    -E) ENTRY="$2"; shift 2;;
    -T) PROFILE="$2"; shift 2;;
    -g) GRID="$2"; shift 2;;
    -w) WORDS="$2"; shift 2;;
    -o) OUTIDX="$2"; shift 2;;
    --reference-only) REFONLY=1; shift;;
    *) break;;
  esac
done
SRC="$1"
[ -n "${SRC:-}" ] && [ -f "$SRC" ] || { echo "usage: diff.sh [-E entry] [-T profile] file.hlsl"; exit 2; }
NAME=$(basename "$SRC" .hlsl)
WORK="build/hlsl-diff/$NAME"
mkdir -p "$WORK"

# ---- reference side: DXC -> SPIR-V -> spirv-cross -> MSL -> metallib ----
if ! command -v dxc >/dev/null 2>&1; then
  export PATH="$PATH:/Volumes/AverySSD/binc/third_party/DirectXShaderCompiler/build/bin"
fi
if ! command -v spirv-cross >/dev/null 2>&1; then
  echo "FAIL: spirv-cross not on PATH (brew install spirv-cross)"; exit 2
fi
dxc -E "$ENTRY" -T "$PROFILE" -spirv -fspv-target-env=vulkan1.2 \
    -fvk-use-dx-layout "$SRC" -Fo "$WORK/ref.spv" || { echo "FAIL: dxc rejected $SRC"; exit 1; }
spirv-cross "$WORK/ref.spv" --msl --msl-version 30000 --entry "$ENTRY" \
    --output "$WORK/ref.metal" || { echo "FAIL: spirv-cross"; exit 1; }
DEVELOPER_DIR="$DEV" xcrun metal -c "$WORK/ref.metal" -o "$WORK/ref.air" 2>"$WORK/ref.metal.log" \
    || { echo "FAIL: metal rejected the MSL (see $WORK/ref.metal.log)"; exit 1; }
DEVELOPER_DIR="$DEV" xcrun metallib "$WORK/ref.air" -o "$WORK/ref.metallib" || exit 1

# spirv-cross renames entry points that collide with Metal's reserved 'main'
# (main -> main0); each side needs its own kernel name in the spec.
KERN=$(grep -oE "kernel void [A-Za-z_][A-Za-z0-9_]*" "$WORK/ref.metal" | head -1 | awk '{print $3}')
[ -n "${KERN:-}" ] || { echo "FAIL: no kernel found in generated MSL"; exit 1; }

printf "kernel %s\ngrid %d\nout %d %d\ndump %d\n" "$KERN" "$GRID" "$OUTIDX" "$WORDS" "$OUTIDX" > "$WORK/ref.spec"
printf "kernel %s\ngrid %d\nout %d %d\ndump %d\n" "$ENTRY" "$GRID" "$OUTIDX" "$WORDS" "$OUTIDX" > "$WORK/ours.spec"

run_ref() {
  binc/harness "$WORK/ref.metallib" "$WORK/ref.spec" 2>/dev/null | grep "^buf$OUTIDX:" || { echo "FAIL: reference dispatch"; exit 1; }
}
REF_OUT=$(run_ref)
echo "reference: $REF_OUT"

[ "$REFONLY" = 1 ] && { echo "OK (reference-only)"; exit 0; }

# ---- ours side: BinC HLSL frontend ----
if ! binc/binc -E "$ENTRY" -T "$PROFILE" "$SRC" -o "$WORK/ours.metallib" 2>"$WORK/ours.log"; then
  echo "FAIL: ours compilation"
  echo "  ours.log: $(head -1 "$WORK/ours.log")"
  exit 1
fi
OURS_OUT=$(binc/harness "$WORK/ours.metallib" "$WORK/ours.spec" 2>/dev/null | grep "^buf$OUTIDX:")
echo "ours:      $OURS_OUT"

python3 - "$REF_OUT" "$OURS_OUT" <<'EOF'
import sys
def words(line):
    return [float(x) for x in line.split()[1:]]
r, o = words(sys.argv[1]), words(sys.argv[2])
if len(r) != len(o):
    print(f"FAIL: word count {len(r)} vs {len(o)}"); sys.exit(1)
bad = 0
for i, (a, b) in enumerate(zip(r, o)):
    tol = 1e-4 * (abs(a) + 1.0)
    if abs(a - b) > tol:
        print(f"  buf[{i}]: ref {a!r} ours {b!r}"); bad += 1
if bad:
    print(f"FAIL: {bad} words differ"); sys.exit(1)
print(f"OK: {len(r)} words match within tolerance")
EOF
