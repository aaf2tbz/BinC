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

# ---- vertex data (VTX_OURS/VTX_REF layouts, VERT words) ----
# optional constant buffer (CBUF="<float words>") and texture (TEX=w:h):
# the cbuffer is bound at index 0, the vertex data moves to index 1
CBUF="${CBUF:-}"; TEX="${TEX:-}"
VBO=0
CBUF_LINES=""; CBUF_REF_LINES=""
if [ -n "$CBUF" ]; then
  VBO=1
  CBUF_LINES="buf ${CBUF_OURS:-0} $CBUF"
  REF_BIDX=$(grep -oE "\[\[buffer\([0-9]+\)\]\]" "$WORK/ref_ps.metal" | grep -oE "[0-9]+" | head -1)
  CBUF_REF_LINES="buf ${REF_BIDX:-0} $CBUF"
fi
TEX_LINES=""
if [ -n "$TEX" ]; then
  REF_TIDX=$(grep -oE "\[\[texture\([0-9]+\)\]\]" "$WORK/ref_ps.metal" | grep -oE "[0-9]+" | head -1)
  TEX_LINES="tex ${TEX_OURS:-0} ${TEX%:*} ${TEX#*:}
tex ${REF_TIDX:-0} ${TEX%:*} ${TEX#*:}"
fi

# reference attribute locations (DXC SPIR-V locations, spirv-cross keeps them)
REF_ATTRS=$(grep -oE "\[\[attribute\([0-9]+\)\]\]" "$WORK/ref_vs.metal" | grep -oE "[0-9]+" | tr '\n' ' ')
set -- $REF_ATTRS
: "${VTX_REF:=vtx $1 $VBO 0 float4
vtx $2 $VBO 16 float4}"
: "${VTX_OURS:=vtx 0 $VBO 0 float4
vtx 8 $VBO 16 float4}"
: "${VERT:=-0.5 -0.5 0.0 1.0 1.0 0.0 0.0 1.0 -0.0 0.5 0.0 1.0 0.0 1.0 0.0 1.0 0.5 -0.5 0.0 1.0 0.0 0.0 1.0 1.0}"

DUMP_LINES="dumppix 0"
if [ "$NRT" -gt 1 ]; then DUMP_LINES="$DUMP_LINES
dumppix 1"; fi
printf 'vertex %s\nfragment %s\nrender %d %d %d\ndraw 3\n%s\n%s\n%s\nbuf %d %s\n%s\n' "$VS" "$PS" "$W" "$H" "$NRT" "$VTX_OURS" "$CBUF_LINES" "$TEX_LINES" "$VBO" "$VERT" "$DUMP_LINES" > "$WORK/ours.spec"
printf 'vertex %s\nfragment %s\nrender %d %d %d\ndraw 3\n%s\n%s\n%s\nbuf %d %s\n%s\n' "$VS" "$PS" "$W" "$H" "$NRT" "$VTX_REF" "$CBUF_REF_LINES" "$TEX_LINES" "$VBO" "$VERT" "$DUMP_LINES" > "$WORK/ref.spec"

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
