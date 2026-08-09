# SM6 feature matrix — corpus-driven status

This matrix is maintained from the vendored Unreal Engine and DirectX Graphics Samples source trees. A feature is not considered supported merely because it parses: it must have an HLSL regression, accepted AIR, and a differential GPU proof where a compute or render fixture is practical.

| Feature | Corpus evidence | Status | Scope / decision | Required proof |
|---|---|---|---|---|
| `f32tof16` / `f16tof32` | Extensive UE packing helpers; D3D12 MiniEngine post-process and raytracing samples | **Implemented; re-sweep pending** | Scalar plus `float2/3/4` / `uint2/3/4` conversion through LLVM `half` bitcasts. Preserve only the low 16 bits on unpack. | `half_pack_intrinsics.hlsl` differential and AIR scalar/vector bitcast probe |
| Binary integer intrinsics | UE uses `firstbithigh`, `firstbitlow`, `countbits` broadly | **Implemented** | LLVM `ctlz`/`cttz`/`ctpop`; re-audit required after `2eaef84`. | existing bit-intrinsic regression and UE re-sweep |
| Wave/quad intrinsics | Large UE `WaveOpUtil.ush` / `WaveBroadcastIntrinsics.ush` surface; D3D12 SM6 wave sample | **Pending design/probe** | Implement only a Metal SIMD-group-mappable subset after exact AIR and runtime probes. Recursion helpers are not a valid initial target. | minimal `WaveGetLane*`/broadcast/reduction differential and stage legality tests |
| `min16*` / `float16_t` | UE FP16, lane-vectorization, TSR and Nanite paths | **Partial** | Existing `half` storage/lowering is present. Add only corpus-proven aliases, overload ranking, and vector packing needs. | type/overload regression plus a GPU fixture |
| `int64_t` / `uint64_t` and 64-bit atomics | Present in SM6/D3D12 survey candidates | **Pending probe** | Separate ordinary integer arithmetic from atomics. Do not infer atomic availability from scalar arithmetic. | distinct AIR probes and differential tests |
| `SV_Barycentrics`, `SV_ViewID`, `SV_ShadingRate` | Present in UE/D3D12 system-value survey | **Pending mapping** | Requires a validated Metal stage input/system-value equivalent; otherwise specific unsupported diagnostics. | AIR metadata probe and render fixture |
| Descriptor heaps / bindless | `ResourceDescriptorHeap` / `SamplerDescriptorHeap` and UE bindless paths occur | **Pending resource-model design** | BinC's static resource-argument lowering must not fake dynamic descriptor indexing. Either implement a real representation or diagnose unsupported usage. | dynamic-index fixture or negative test |
| Texture compare/gather methods | Expected in SM6 resource paths | **Pending corpus bucket** | Implement only actual high-frequency failures after fresh audits. | reference render/compute differential |
| Mesh/amplification shaders | D3D12 MeshShader sample sources | **Explicitly unsupported** | No correct Metal mesh pipeline in the current AST/AIR/harness model. Parse/classify and report a stage-specific diagnostic. | `as_*`/`ms_*` negative tests |
| Ray tracing / DXR | UE and D3D12 raytracing libraries/samples | **Explicitly unsupported** | No ray-generation/hit/miss callable-shader or acceleration-structure pipeline in BinC. | `lib_*`/DXR negative tests |

## Guardrails

- Keep matrix decisions source- and probe-backed; update this table after each family audit.
- Parser acceptance for mesh/DXR is not codegen support.
- Any feature marked implemented must be added to the standard local gate, not only manually exercised.
- Resource binding specs for new render fixtures derive indices from emitted AIR metadata.
