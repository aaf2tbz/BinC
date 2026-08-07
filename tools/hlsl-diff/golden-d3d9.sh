#!/bin/bash
# tools/hlsl-diff/golden-d3d9.sh — D3D9 .fx golden test driver.
#
# Goldens are the Phase-7 verification path for sm3/effects shaders where no
# clean differential reference exists (fxc is Windows-only): the shader is
# compiled with the BinC HLSL frontend, rendered through the harness with a
# hand-built spec, and the pixel dump is compared against a PINNED golden
# (tools/hlsl-diff/goldens/<name>/expected_pix0.txt) that was hand-verified
# once against the shader's documented math.
#
# usage: golden-d3d9.sh <golden-name>
#   goldens/<name>/config.sh   — sourced: SHADER, VS_ENTRY, PS_ENTRY
#   goldens/<name>/spec.py     — writes the harness spec (CBUF offsets are
#                                parsed from the emitted .ll, not hardcoded)
#   goldens/<name>/expected_pix0.txt — the pinned pixel dump
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
NAME="${1:?usage: golden-d3d9.sh <golden-name>}"
G="tools/hlsl-diff/goldens/$NAME"
[ -f "$G/config.sh" ] || { echo "FAIL: no $G/config.sh"; exit 2; }
# shellcheck disable=SC1090
. "$G/config.sh"
WORK="build/hlsl-diff/golden-$NAME"
mkdir -p "$WORK"

# ---- compile: both stages into one library (stage inferred from semantics) ----
if ! binc/binc -E "$PS_ENTRY" -T ps_2_0 --stage-all "$G/$SHADER" -o "$WORK/ours.metallib" 2>"$WORK/ours.log"; then
  echo "FAIL: compilation"; echo "  $(head -1 "$WORK/ours.log")"; exit 1
fi
# the .ll is emitted as <shader>.ll in the CWD; copy it for spec.py
cp "$ROOT/$(basename "$SHADER" .fx).ll" "$WORK/ours.ll" 2>/dev/null \
  || { echo "FAIL: no .ll emitted (compile aborted before AIR)"; exit 1; }

# ---- spec: generated per-golden (CBUF offsets parsed from the .ll) ----
python3 "$G/spec.py" "$WORK/ours.ll" "$WORK/ours.spec" || { echo "FAIL: spec generation"; exit 1; }

OUT=$(binc/harness "$WORK/ours.metallib" "$WORK/ours.spec" 2>/dev/null | grep "^pix0:")
[ -n "$OUT" ] || { echo "FAIL: harness dispatch"; exit 1; }
echo "$OUT" > "$WORK/pix0.txt"

python3 - "$WORK/pix0.txt" "$G/expected_pix0.txt" <<'EOF'
import sys
def words(path):
    return [float(x) for x in open(path).read().split()[1:]]
got, exp = words(sys.argv[1]), words(sys.argv[2])
if len(got) != len(exp):
    print(f"FAIL: word count {len(got)} vs {len(exp)}"); sys.exit(1)
bad = 0
for i, (a, b) in enumerate(zip(got, exp)):
    tol = 1e-4 * (abs(b) + 1.0)
    if abs(a - b) > tol:
        bad += 1
        if bad <= 5:
            print(f"  pix[{i}]: got {a!r} exp {b!r}")
if bad:
    print(f"FAIL: {bad} words differ from the pinned golden"); sys.exit(1)
nz = sum(1 for v in got if v != 0)
if nz < 256:
    print(f"FAIL: only {nz} nonzero words (render looks empty)"); sys.exit(1)
print(f"OK: {len(got)} words match the pinned golden ({nz} nonzero)")
EOF
