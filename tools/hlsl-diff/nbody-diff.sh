#!/bin/bash
# tools/hlsl-diff/nbody-diff.sh — Phase-5 compute differential: the D3D12
# nBodyGravity gravity CS (groupshared + barriers + 4 thread semantics + inout
# helper + cbuffer + structured buffers), ours vs DXC->SPIRV-Cross on GPU.
set -u
SRC="third_party/DirectX-Graphics-Samples/Samples/Desktop/D3D12nBodyGravity/src/nBodyGravityCS.hlsl"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
WORK="build/hlsl-diff/nbody"
mkdir -p "$WORK"
DEV="${DEV:-/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer}"
export PATH="$PATH:$PWD/third_party/DirectXShaderCompiler/build/bin"

# ---- reference: DXC -> SPIRV-Cross MSL -> metallib ----
dxc -E CSMain -T cs_5_0 -spirv -fspv-target-env=vulkan1.2 -fvk-use-dx-layout -fvk-b-shift 0 0 -fvk-t-shift 10 0 -fvk-u-shift 20 0 -Fo "$WORK/ref.spv" "$SRC" 2>"$WORK/refdxc.log" || { echo "FAIL: dxc"; cat "$WORK/refdxc.log"; exit 1; }
spirv-cross "$WORK/ref.spv" --msl --msl-version 30000 --entry CSMain --output "$WORK/ref.metal" 2>"$WORK/refsc.log" || { echo "FAIL: spirv-cross"; cat "$WORK/refsc.log"; exit 1; }
DEVELOPER_DIR="$DEV" xcrun metal -c "$WORK/ref.metal" -o "$WORK/ref.air" 2>"$WORK/refmetal.log" || { echo "FAIL: metal ref"; head -3 "$WORK/refmetal.log"; exit 1; }
DEVELOPER_DIR="$DEV" xcrun metallib "$WORK/ref.air" -o "$WORK/ref.metallib" || { echo "FAIL: metallib ref"; exit 1; }
# per-side kernel names: spirv-cross renames main -> main0
REFKERN=$(grep -oE "kernel void [A-Za-z0-9_$]+" "$WORK/ref.metal" | awk '{print $3}' | head -1)
[ -z "$REFKERN" ] && REFKERN="CSMain0"

# ---- ours ----
binc/binc -E CSMain -T cs_5_0 "$SRC" -o "$WORK/ours.metallib" 2>"$WORK/ours.log" || { echo "FAIL: ours"; head -2 "$WORK/ours.log"; exit 1; }

# ---- shared spec, per-side kernel name ----
python3 tools/hlsl-diff/nbody-spec.py > "$WORK/spec.tmpl"
sed "s/kernel CSMain/kernel $REFKERN/" "$WORK/spec.tmpl" > "$WORK/ref.spec"
cp "$WORK/spec.tmpl" "$WORK/ours.spec"

REF_OUT=$(binc/harness "$WORK/ref.metallib" "$WORK/ref.spec" 2>/dev/null | grep "^buf2" | cut -d' ' -f2-)
OURS_OUT=$(binc/harness "$WORK/ours.metallib" "$WORK/ours.spec" 2>/dev/null | grep "^buf2" | cut -d' ' -f2-)
if [ -z "$REF_OUT" ] || [ -z "$OURS_OUT" ]; then echo "FAIL: no output (ref=${REF_OUT:+yes} ours=${OURS_OUT:+yes})"; exit 1; fi
echo "reference: ${REF_OUT:0:120}..."
echo "ours:      ${OURS_OUT:0:120}..."
python3 - "$REF_OUT" "$OURS_OUT" <<'EOF'
import sys
r = [float(x) for x in sys.argv[1].split()]
o = [float(x) for x in sys.argv[2].split()]
if len(r) != len(o):
    print(f"FAIL: word count {len(r)} vs {len(o)}"); sys.exit(1)
bad = 0
for i, (a, b) in enumerate(zip(r, o)):
    # O(N^2) accumulation noise: 3e-3 relative (the exact cancellation of the
    # symmetric ring differs in the last ULPs between the two compilations)
    tol = 3e-3 * (abs(a) + 1.0)
    if abs(a - b) > tol:
        print(f"  buf[{i}]: ref {a!r} ours {b!r}"); bad += 1
if bad:
    print(f"FAIL: {bad} words differ"); sys.exit(1)
print(f"OK: {len(r)} words match within tolerance")
EOF
