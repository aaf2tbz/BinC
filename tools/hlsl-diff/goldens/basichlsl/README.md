# BasicHLSL golden — hand-verification record

Shader: DirectX-SDK-Samples `BasicHLSL.fx` (RenderSceneVS + RenderScenePS,
`vs_2_0`/`ps_2_0` compile lines, compiled with `--stage-all`).

## Configuration (as pinned)

- 64×64 viewport, one triangle: (-0.5,-0.5) (0,0.5) (0.5,-0.5) in NDC,
  normal (0,0,1), uv (1,0)/(0,1)/(0,0).
- Harness gradient texture 64×64: texel(x,y) = (x+1, y+1, x+y+1, 1).
- `$uniforms` (320 bytes, LLVM layout):
  - MaterialAmbient (0.2, 0.2, 0.2, 1), MaterialDiffuse (1, 0.8, 0.6, 1)
  - nNumLights = 1; LightDir[0] = normalize(0.5, 0.5, 1) ≈ (0.408248,
    0.408248, 0.816497); LightDiffuse[0] = (1,1,1,1); LightAmbient =
    (0.15, 0.15, 0.15, 1); g_fTime = 0; mWorld = mWVP = identity.
  - `uniform int nNumLights` = 1, `uniform bool bTexture` = true,
    `uniform bool bAnimate` = false (per-draw constants folded into
    `$uniforms` — the D3D9 semantic for `uniform` params).
- Bindings: vertex buffer 1 (pos locn0, normal locn4, uv locn1), `$uniforms`
  at vertex buffer 2 and fragment buffer 5, texture at fragment 3.

## Math (hand-verified against the rendered pixels)

    n = (0,0,1);  dot(n, LightDir[0]) = 0.816497
    vTotalLightDiffuse = LightDiffuse[0] * 0.816497 = 0.816497
    Diffuse.rgb = MaterialDiffuse * 0.816497 + MaterialAmbient * LightAmbient
                = (0.816497, 0.653198, 0.489898) + (0.03, 0.03, 0.03)
                = (0.846497, 0.683198, 0.519898)          a = 1.0
    output = tex2D(uv) * Diffuse

Spot checks from the pinned dump (pixel → texel → product):
pix(23,4) = (2.5395, 42.3582, 33.2735, 1.0):
  texel (3.0, 62.0, 64.0, 1.0) × Diffuse = (2.5395, 42.3582, 33.2729, 1.0) ✓
pix(24,4) = (0.8465, 42.3582, 32.2337, 1.0):
  texel (1.0, 62.0, 62.0, 1.0) × Diffuse = (0.8465, 42.3582, 32.2336, 1.0) ✓
pix(39,4) = (3.386, 40.9919, 32.7536, 1.0):
  texel (4.0, 60.0, 63.0, 1.0) × Diffuse = (3.386, 40.9919, 32.7536, 1.0) ✓

## Codegen exercised

D3D9 semantics table (POSITION→locn0, NORMAL→locn4, TEXCOORDn→n+1,
COLOR0→locn8, VS-out POSITION→air.position), float3/float4 uniform arrays
(register-aligned), `uniform` params → `$uniforms`, tex2D + sampler_state
Texture capture, implicit vector truncation (dot(float3, float4)),
row-major cbuffer matrices with the mul transpose, matrix by-value in
struct fields, for-loop with a uniform bound, bool branch on a uniform.
