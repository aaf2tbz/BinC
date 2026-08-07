# PixelMotionBlur.fx (D3D9 SDK) — 2-pass technique golden (golden 3).
# Hand-verified 2026-08-07 with identity matrices:
#   P0 (WorldPixelShaderColor) = tex2D(MeshTexture) * Diffuse,
#     Diffuse = 1*1*max(0,dot(N,L)) + 0.1*1 = 0.577350 + 0.1 = 0.677350
#     -> pixel = texel(x+1, y+1, x+y+1) * 0.677350, alpha 1
#   P1 (WorldPixelShaderVelocity) = (0,0,1,1) everywhere (curr == last frame)
# Exercises: 2-pass technique (same VS, two PS), direct semantic fragment
# params (packed to a synthetic stage-in struct), struct VS_OUTPUT/PS_OUTPUT,
# (float3x3) matrix cast, vector /= vector, max(0, dot()), .rgb swizzle-assign
# with a float4 RHS, static const, MRT-style PS_OUTPUT (COLOR0+COLOR1) staged
# via struct-return COLOR semantics.
SHADER="shader.fx"
VS_ENTRY="WorldVertexShader"
PROFILE="vs_2_0"
PASSES="P0 P1"
PS_P0="WorldPixelShaderColor"
PS_P1="WorldPixelShaderVelocity"
