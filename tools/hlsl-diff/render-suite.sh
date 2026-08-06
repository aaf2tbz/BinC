#!/bin/bash
# tools/hlsl-diff/render-suite.sh — the full Phase-3/4 render differential suite:
# every corpus VS/PS pair + cbuffer + texture + MRT test, ours vs the
# DXC->SPIRV-Cross reference, pixel-compared on GPU.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
FAIL=0

run() { # name, then env-prefixed render-diff args
  local name="$1"; shift
  echo "== $name"
  if ! bash tools/hlsl-diff/render-diff.sh "$@" > /tmp/rd_$name.log 2>&1; then
    echo "   FAIL: $(tail -1 /tmp/rd_$name.log)"; FAIL=1
  else
    echo "   ok"
  fi
}

# corpus pairs: position+color layout (defaults)
for t in HelloTriangle HelloFrameBuffering HelloBundles; do
  run "$t" "third_party/DirectX-Graphics-Samples/Samples/Desktop/D3D12HelloWorld/src/$t/shaders.hlsl"
done

# constant buffer: cbuffer b0 at index 0, vertex data at index 1
CBUF="0.2 0.2 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0" \
  run HelloConstBuffers "third_party/DirectX-Graphics-Samples/Samples/Desktop/D3D12HelloWorld/src/HelloConstBuffers/shaders.hlsl"
unset CBUF

# tight D3D register packing (float + packed float2 + float4 crossing a
# register boundary); the PS reads the cbuffer so the fragment needs it at
# its own buffer index (ours: 2 — stage-in args come first)
CBUF="1.0 2.0 3.0 0.0 4.0 5.0 6.0 7.0 8.0 9.0 0.0 0.0 10.0 11.0 12.0 0.0" CBUF_OURS=2 \
  run CBufferPack tools/hlsl-diff/tests/render/cbuffer_pack.hlsl
unset CBUF CBUF_OURS

# texture sample: position+uv layout, 4x4 gradient texture (ours texture arg 2)
TEX=4:4 TEX_OURS=2 \
VERT="-0.5 -0.5 0.0 1.0 0.0 0.0 -0.0 0.5 0.0 1.0 1.0 0.0 0.5 -0.5 0.0 1.0 0.0 1.0" \
VTX_OURS="vtx 0 0 0 float4
vtx 1 0 16 float2" \
VTX_REF="vtx 0 0 0 float4
vtx 1 0 16 float2" \
  run HelloTexture "third_party/DirectX-Graphics-Samples/Samples/Desktop/D3D12HelloWorld/src/HelloTexture/shaders.hlsl"

# MRT: two render targets (base + swizzled key)
NRT=2 run MRT tools/hlsl-diff/tests/render/mrt.hlsl

[ "$FAIL" = 0 ] && echo "render-suite: all pairs pixel-identical" || { echo "render-suite: FAILURES"; exit 1; }
