# BumpDirectionalLight.fx (GDC 2005 HLSL workshop) — normal-mapped mesh, golden 2.
# Hand-verified 2026-08-07: 512 lit pixels, values == tex2D(normalmap) driven
# per-pixel lighting × diffuse texture + reflectionRatio × texCUBE(env) + specular:
#   pix(23,4) = (6.1, 65.3, 70.3, 1) — normal-map perturbation visible (r varies
#   6.1/4.4/7.1 across pixels while g/b track the gradient × lighting).
# Exercises: NORMAL/TANGENT/BINORMAL vertex locns (4/5/6), user struct stage-in/out,
# textureCUBE + samplerCUBE-style binding (paren sampler_state), tex2D/texCUBE
# rewrites, inverse() (View identity), mul(float3, float4x4) extension, matrix
# uniforms incl. a nested SasDirectionalLight struct in $uniforms, specular pow.
SHADER="shader.fx"
VS_ENTRY="VS"
PS_ENTRY="PS"
PROFILE="vs_2_0"
