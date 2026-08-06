#!/bin/bash
# tools/hlsl-diff/render-diff.sh — Phase-3 render differential.
#
# Compiles a VS+PS pair both ways (ours: BinC HLSL frontend --stage-all;
# reference: DXC -> SPIRV-Cross -> MSL), renders the same triangle with the
# same vertex data, and compares the rendered pixels word-for-word.
#
# Usage: render-diff.sh shaders.hlsl   (entry points must be VSMain/PSMain)
set -u
SRC="${1:?usage: render-diff.sh shaders.hlsl}"
W="${RW:-64}"; H="${RH:-64}"; NRT="${NRT:-1}"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
WORK="build/hlsl-diff/render-$(echo "$SRC" | tr '/' '_' | sed 's/\.hlsl$//')"
mkdir -p "$WORK"
DEV="${DEV:-/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer}"
export PATH="$PATH:$PWD/third_party/DirectXShaderCompiler/build/bin"
VS="${2:-VSMain}"; PS="${3:-PSMain}"

# ---- reference: DXC -> SPIR-V -> SPIRV-Cross MSL -> metallib ----
dxc -E "$VS" -T vs_6_0 -spirv -fspv-target-env=vulkan1.2 -fvk-use-dx-layout -Fo "$WORK/ref_vs.spv" "$SRC" 2>"$WORK/refdxc_vs.log" || { echo "FAIL: dxc vs"; cat "$WORK/refdxc_vs.log"; exit 1; }
dxc -E "$PS" -T ps_6_0 -spirv -fspv-target-env=vulkan1.2 -fvk-use-dx-layout -Fo "$WORK/ref_ps.spv" "$SRC" 2>"$WORK/refdxc_ps.log" || { echo "FAIL: dxc ps"; cat "$WORK/refdxc_ps.log"; exit 1; }
spirv-cross "$WORK/ref_vs.spv" --msl --msl-version 30000 --entry "$VS" --output "$WORK/ref_vs.metal" 2>"$WORK/refsc_vs.log" || { echo "FAIL: spirv-cross vs"; cat "$WORK/refsc_vs.log"; exit 1; }
spirv-cross "$WORK/ref_ps.spv" --msl --msl-version 30000 --entry "$PS" --output "$WORK/ref_ps.metal" 2>"$WORK/refsc_ps.log" || { echo "FAIL: spirv-cross ps"; cat "$WORK/refsc_ps.log"; exit 1; }
cat "$WORK/ref_vs.metal" "$WORK/ref_ps.metal" > "$WORK/ref_all.metal"
DEVELOPER_DIR="$DEV" xcrun metal -c "$WORK/ref_all.metal" -o "$WORK/ref.air" 2>"$WORK/refmetal.log" || { echo "FAIL: metal ref"; head -3 "$WORK/refmetal.log"; exit 1; }
DEVELOPER_DIR="$DEV" xcrun metallib "$WORK/ref.air" -o "$WORK/ref.metallib" 2>"$WORK/reflink.log" || { echo "FAIL: metallib ref"; exit 1; }

# ---- ours: BinC HLSL frontend, both stages inferred from semantics ----
if ! binc/binc -E "$VS" -T vs_6_0 --stage-all "$SRC" -o "$WORK/ours.metallib" 2>"$WORK/ours.log"; then
  echo "FAIL: ours compilation"; echo "  ours.log: $(head -1 "$WORK/ours.log")"; exit 1
fi

# ---- vertex data: 3 vertices, position (float4) + color (float4) interleaved ----
VERT="-0.5 -0.5 0.0 1.0 1.0 0.0 0.0 1.0 -0.0 0.5 0.0 1.0 0.0 1.0 0.0 1.0 0.5 -0.5 0.0 1.0 0.0 0.0 1.0 1.0"

# reference attribute locations (DXC SPIR-V locations, spirv-cross keeps them)
REF_ATTRS=$(grep -oE "\[\[attribute\([0-9]+\)\]\]" "$WORK/ref_vs.metal" | grep -oE "[0-9]+" | tr '\n' ' ')
set -- $REF_ATTRS
REF_VTX="vtx $1 0 0 float4
vtx $2 0 16 float4"
# ours: the BinC mapping (position -> locn0, color -> locn8)
OURS_VTX="vtx 0 0 0 float4
vtx 8 0 16 float4"

printf 'vertex %s\nfragment %s\nrender %d %d %d\ndraw 3\n%s\nbuf 0 %s\ndumppix 0\ndumppix 1\n' "$VS" "$PS" "$W" "$H" "$NRT" "$OURS_VTX" "$VERT" > "$WORK/ours.spec"
printf 'vertex %s\nfragment %s\nrender %d %d %d\ndraw 3\n%s\nbuf 0 %s\ndumppix 0\ndumppix 1\n' "$VS" "$PS" "$W" "$H" "$NRT" "$REF_VTX" "$VERT" > "$WORK/ref.spec"

REF_OUT=$(binc/harness "$WORK/ref.metallib" "$WORK/ref.spec" 2>/dev/null | grep "^pix")
OURS_OUT=$(binc/harness "$WORK/ours.metallib" "$WORK/ours.spec" 2>/dev/null | grep "^pix")
echo "reference: ${REF_OUT:0:120}..."
echo "ours:      ${OURS_OUT:0:120}..."

if [ "$REF_OUT" = "$OURS_OUT" ]; then
  echo "OK: rendered pixels identical"
  exit 0
fi
# tolerance compare: same word count + per-word float tolerance
rf=($REF_OUT); of=($OURS_OUT)
if [ ${#rf[@]} -ne ${#of[@]} ]; then echo "FAIL: pixel count $((${#of[@]}-1)) vs $((${#rf[@]}-1))"; exit 1; fi
diffs=0
for i in $(seq 1 $((${#rf[@]}-1))); do
  a=$(python3 -c "print(abs(${rf[$i]}-${of[$i]}))" 2>/dev/null)
  tol=$(python3 -c "print(1e-4*(abs(${rf[$i]})+1.0))" 2>/dev/null)
  ok=$(python3 -c "print(1 if float('$a')<=float('$tol') else 0)" 2>/dev/null)
  if [ "$ok" != "1" ]; then diffs=$((diffs+1)); fi
done
if [ "$diffs" -eq 0 ]; then echo "OK: rendered pixels match within tolerance"; exit 0; fi
echo "FAIL: $diffs pixels differ"
exit 1
