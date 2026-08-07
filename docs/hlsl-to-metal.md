# HLSL → Metal: BinC's DirectX frontend (mapping guide)

BinC compiles Direct3D shader sources — HLSL for D3D10/11/12 (sm4+) and the
D3D9 sm3 + `.fx` effects subset — directly to `.metallib` through the same
AIR backend used for the `.binc` language. This document is the mapping guide:
what compiles, how it maps, and what is explicitly unsupported.

The frontend is a **lowering bridge**: HLSL AST → the shared BinC AST → the
existing AIR backend, untouched. See `.hermes/plans/2026-08-05_163031-hlsl-to-metal.md`
for the phase history and `docs/hlsl-semantics.md` for the authoritative
semantic↔Metal table (this guide summarizes it).

## Driver surface

```
binc -E <entry> -T <profile> file.hlsl -o out.metallib
```

- `-T` profiles: `vs_*` → vertex, `ps_*` → fragment, `cs_*` → compute kernel,
  `gs_*` → geometry (parses + classifies; lowering is **unsupported**, see
  below). sm3/sm2 profiles select D3D9 semantics.
- `.hlsl`/`.fx`/`.fxh` route to this frontend; everything else is the `.binc`
  frontend.
- `--stage-all`: lower EVERY function whose return semantics imply a stage
  into one library (a VS+PS pair compiles into one `.metallib` — metallib
  cannot merge two libraries). Stage inference per function:
  - return semantic `SV_Target`/`COLOR` (direct or struct field) → fragment
  - return semantic `SV_Position`/`POSITION` (direct or struct field) → vertex
  - under a `gs_*` profile, semantic-less functions (stream objects) → geometry
  - otherwise (non-entry, no semantics) → internal function
- `--emit-ll`: stop after AIR emission (no metal front-end).

## Semantics

The full table is `docs/hlsl-semantics.md`. Highlights:

| Construct | Mapping |
|---|---|
| VS in `POSITION` | `[[attribute(0)]]` |
| VS in `TEXCOORDn` | locn n+1 (locn0 stays for POSITION) |
| VS in `NORMAL`/`TANGENT`/`BINORMAL` | locns 4/5/6 |
| VS in `COLORn` | locn 8+n |
| VS out `POSITION` / PS in `SV_Position` | `[[position]]` |
| PS out `COLORn` / `SV_TargetN` / `TEXCOORDn` | `[[color(N)]]` |
| `SV_DispatchThreadID` / `SV_GroupThreadID` / `SV_GroupID` / `SV_GroupIndex` | coord3D param rewrites (`.global`/`.local`) |
| D3D9 VS/PS user structs (`VSInput input`, `PS_INPUT In`) | unpacked directly as stage params, per-field semantics |
| D3D9 direct semantic params (`float4 Diffuse : COLOR0`) | packed into a synthetic `<fn>$in` stage-in struct |

Stage-in and stage-out locn conventions differ for D3D9 (vertex inputs use
NORMAL→4 while vertex outputs / fragment inputs use NORMAL→3) — the VS
output locns must equal the PS input locns for the rasterizer to pass them;
the lowering guarantees this.

## Constant buffers and uniforms

- `cbuffer` → one `constant` buffer param (`air.buffer`, address space 2);
  field references rewrite to struct-field access.
- **D3D9 uniform globals and `uniform` params** (per-draw constants) pack into
  one synthetic `%struct.$uniforms` const struct; the harness/host binds it at
  the register index following the declared resources. Rules:
  - float2/float3 uniforms occupy a **full float4 register** (16 bytes,
    D3D9 constant-register alignment; floatN arrays align each element);
  - struct-typed globals pack as nested struct fields (recursive layout,
    v3f/v4f 16-byte alignment);
  - `groupshared` globals are **excluded** (they stay threadgroup memory);
  - `static const` globals are not packed.
- `register(bN/tN/uN)` is parsed for layout but binding is by argument
  position (register parity lands with the reflection sidecar).

## Matrices

- Row-major convention: `mul(v, M)` = row-vector × matrix (v·M, M·v transposed)
  — matches D3D9's `mul()` semantics; the differential harness cross-checks
  every cbuffer crossing against DXC→SPIRV-Cross.
- `mul(floatN, floatMxM)`: implicit vector extension to float4 (w=1) when the
  vector is narrower than the matrix (D3D9), truncated back at storage.
- `inverse()` is a standard intrinsic (cofactor adjugate/determinant, 3x3/4x4).
- `(float3x3)M` casts narrow a matrix's rows.

## Textures and samplers

- `texture`/`texture2D`/`textureCUBE` globals + `sampler`/
  `sampler_state { Texture = <t>; }` bind the texture to the sampler; the
  `Texture = <t>` **or** `Texture = (t)` forms are captured.
- `tex2D(s, uv)` → `t.Sample(s, uv)`; `texCUBE` → `air.sample_texture_cube`;
  `tex2Dgrad` → SampleLevel; `tex2Dproj`/`tex2Dlod` → SampleLevel with the
  projection/lod in the last lane.
- Sampler-state filter/address modes are parsed (not executed); the harness
  binds nearest/clamp defaults.

## Numeric behavior

- `half` is real fp16 **at storage**: computes run in f32, stores round to
  fp16 (fptrunc) — matching fxc's storage semantics; loads widen (fpext).
  Overload resolution accepts implicit float→half.
- Implicit vector truncation is D3D9-legal: `float3 += float4 * s`,
  `s.f.rgb = float4_expr`, `float2 v = float4 - float4` — the wider operand
  truncates at the storage boundary (and inside the expression where a
  composite intrinsic's width demands it, e.g. `dot(float3, float4)`).
- No implicit bool→float; `max(0, dot(...))` resolves via int→float promotion.

## Verification story

- **Differential-first**: every lowering is cross-checked on a real Apple GPU
  against DXC → SPIR-V → spirv-cross → MSL of the same source
  (`tools/hlsl-diff/diff.sh`, `render-suite.sh`, `nbody-diff.sh`).
- **Goldens** (no clean differential path for D3D9 — fxc is Windows-only):
  three classic `.fx` samples rendered and pinned with hand-verified pixel
  math (`make golden-d3d9`): BasicHLSL textured, BumpDirectionalLight
  normal-mapped, PixelMotionBlur 2-pass technique.
- **Sweep** (`make diff-sweep`): deterministic suites (hard) + a corpus
  compute probe into `build/hlsl-diff/coverage.md` (intrinsics × semantics ×
  resources per shader); any crash or hang in the probe set fails the gate.
- **Fuzz**: `.binc` + `.hlsl` mutation fuzzing (`make test-fuzz`) — no
  crashes/hangs on either frontend.
- **Negative suite**: `tests/negative/` (.binc), `tests/negative/*.fx` (GS
  rejection), `tests/hlsl/negative/` (located diagnostics for bad code).

## Unsupported (documented, diagnosed, tested)

| Feature | Behavior |
|---|---|
| Geometry shaders (`gs_*`, `TriangleStream`, `[maxvertexcount]`) | parses + stage-classifies, then a located error: *"geometry shader lowering (Metal mesh) not yet wired"* (D3D10-era, out of the D3D9 scope; Metal's mesh stage is the eventual target) |
| `RWStructuredBuffer`/`AppendStructuredBuffer` method forms (`Load`, `Append`, `IncrementCounter`) | located "unsupported atomic method" error (plain element access + `Interlocked*` work) |
| Multiple `inout` params on one function | located error (single-inout value-return rewrite supported) |
| D3D9 multi-out-param signatures (`void V(float4 p : POSITION, out float4 o : COLOR0, ...)`) | not yet supported (struct-return style is the supported path) |
| Raytracing shader types (`RaytracingAccelerationStructure`, `[shader("raygen")]`, callables) | parse gap — "expected an expression" class errors |
| `groupshared` inside a function via the uniform packer | fixed to stay threadgroup memory (see sweep regression) |
| Wave intrinsics, `ByteAddressBuffer` full method surface, sm6 features | fail with the frontend's located errors; recorded per-shader in the sweep coverage table |

Every unsupported surface fails with a **located diagnostic**, never a crash:
the fuzzer and the sweep probe enforce that.
