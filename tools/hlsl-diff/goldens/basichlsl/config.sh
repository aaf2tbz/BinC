# BasicHLSL.fx (Direct3D10 SDK sample) — textured teapot route, golden 1.
# Hand-verified 2026-08-07: pixels == tex2D(uv) * Diffuse where
#   Diffuse.rgb = MaterialDiffuse * (LightDiffuse[0] * dot(n, LightDir[0]))
#               + MaterialAmbient * LightAmbient
# with n=(0,0,1), LightDir[0]=normalize(0.5,0.5,1), 1 light, identity WVP:
#   Diffuse = (0.846, 0.683, 0.520, 1.0) — spot-checked at multiple pixels.
SHADER="shader.fx"
VS_ENTRY="RenderSceneVS"
PS_ENTRY="RenderScenePS"
