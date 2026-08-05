# third_party — vendored reference assets

Everything here is **not committed** (`.gitignore`): the corpus is re-cloned
locally and, in CI, by the workflow. License files of each project are kept
in their own tree. All projects below are permissively licensed.

| Directory | Upstream | License | What it is for |
|---|---|---|---|
| `DirectX-Graphics-Samples` | github.com/microsoft/DirectX-Graphics-Samples | MIT | D3D12/11 sample `.hlsl` shaders (HelloTriangle, HelloCompute, ParticleCompute, …) — the primary sm5 corpus |
| `DirectX-SDK-Samples` | github.com/microsoft/DirectX-SDK-Samples | MIT | Classic D3D9-era `.fx` effects, sm3 shaders, and the Media `.dds` texture packs — the D3D9 corpus and real texture art |
| `DirectXShaderCompiler` | github.com/microsoft/DirectXShaderCompiler | Apache-2.0 | DXC source (reference compiler, buildable on macOS) **and** the HLSL lit-test corpus at `tools/clang/test/HLSL` — parser conformance tests |
| `ShaderConductor` | github.com/microsoft/ShaderConductor | MIT | The HLSL→SPIR-V→MSL translator built for Epic's UE Metal backend — reference design for semantics/matrix mapping; its MSL emission is differential ground truth |

## Cloning (sparse by design)

The sample repos are huge; we only need shader sources, `.fx` files, and
textures. The canonical commands (as run by `tools/vendor/clone.sh`):

```bash
git clone --depth 1 https://github.com/microsoft/DirectXShaderCompiler.git
git clone --depth 1 https://github.com/microsoft/ShaderConductor.git
git clone --depth 1 --filter=blob:none --sparse https://github.com/microsoft/DirectX-Graphics-Samples.git
git clone --depth 1 --filter=blob:none --sparse https://github.com/microsoft/DirectX-SDK-Samples.git
# then, in each sample repo:
git sparse-checkout set --no-cone '**/*.hlsl' '**/*.fx' '**/*.fxh' '**/*.dds' '**/LICENSE*' 'README.md'
```

`corpus.json` (regenerate with `make corpus-scan`) records the exact commit of
each vendor and indexes every shader with its best-guess profiles
(`[shader("...")]` attributes, `compile vs_3_0` statements in `.fx` files)
plus every texture.

## Toolchain (reference side of the differential harness)

- `dxc` — built from the vendored source (`make` in
  `DirectXShaderCompiler/build`), or `brew install directx-headers` provides
  the d3d12 headers the build needs. Emits DXIL and SPIR-V.
- `spirv-cross` — `brew install spirv-cross` (prebuilt CLI). Emits MSL from
  SPIR-V.

Recorded versions live in `corpus.json` (`toolchain` section).

## DDS support status

`tools/dds/dds.h` parses the DDS container (classic + DX10 headers) and maps
formats to Metal: BC1–BC7, BC4/BC5, uncompressed BGRA8/RGBA8 (mask-detected),
R8/R16/RG8 (luminance), float RGBA16/RGBA32 (including the D3D9-era
`D3DFMT_*F` fourccs like `A16B16G16R16F` = 113), R5G6B5, and volume textures.
**528 of 536** corpus `.dds` files parse (`make test-dds`); the 8 failures are
degenerate headers documented in `degenerate_dds.txt` (D3D9 "HILO" files whose
headers lie about their bit depth, plus one mangled D3D12 OMM texture).

Note: 24-bit RGB has no native Metal pixel format — reported as
`INVALID_24BIT` (software decode if the corpus ever needs it).
