VERIFIED on this machine (Xcode 27 beta4, metal 32023.921, AIR v18):
1. .air = LLVM bitcode (magic 0xBC C0DE) wrapped by Apple header (0xDE C0 17 0B).
2. `metal -emit-llvm` emits that bitcode; `metal` front-end = patched Clang for target `air64`.
3. `metallib` (AIR-LLD, based on LLD) links .air -> .metallib (final Metal executable).
4. End-to-end MSL -> .air -> .metallib works (hello.metal test).
5. Generic stock-clang bitcode is REJECTED ("Invalid bitcode"); air64 target is NOT in
   stock clang — only in the `metal` driver's patched clang. => BinC must drive `metal`.
6. Address-space qualifiers (device/constant/thread) are MSL keywords, NOT C keywords:
   they require <metal_stdlib> + the MSL language mode. => BinC needs a rewrite/compliance layer.
7. metal-ir-converter is GONE in this toolchain (deprecated; AIR-LLD replaced it).
8. Metal 4 (WWDC25) = new MTL4 core API surface, NOT a new shading language. MSL unchanged.

ADDITIONAL VERIFIED (advanced parallelism shapes, all from compiled MSL -> .ll):
A. Address spaces confirmed: 0=thread/private, 1=device, 2=constant/sampler, 3=threadgroup.
B. Textures: `%struct._texture_2d_t addrspace(1)*`; metadata `!{i, !"air.texture", !"air.location_index", i, !"air.read"|"air.write", ...}`.
   Access via intrinsics `air.read_texture_2d.*` / `air.write_texture_2d.*`; sampler `air.get_read_sampler()` -> addrspace(2).
C. Threadgroup shared memory: module-level globals in addrspace(3), e.g. `[16 x [16 x <4 x float>]] addrspace(3)*`.
D. 2D/3D coords: vector coords `<2 x i16>` (ushort2); built-ins `air.thread_position_in_grid`,
   `air.thread_position_in_threadgroup`, `air.threadgroup_position_in_grid`. Grid dim = vector width.
E. Atomics: wrapper struct `metal::_atomic { T }`; intrinsic `air.atomic.global.add.f32`; arg metadata
   `air.struct_type_info`; scalar accumulator buffer is read_write.
F. Grid extent: `air.threads_per_grid` built-in.
=> BinC's parallelism type system can map 1:1 onto these. See PARALLELISM.md.

RENDER PATH + METAL 4 (verified by compiling MSL -> .ll):
G. MSL standards: metal3.0 / 3.1 / 4.0 / 4.1. Default toolchain target = Metal 4.1 ("Metal",4,1,0).
H. Shader STAGE is a named module metadata, NOT a keyword in IR:
   compute -> !air.kernel ; vertex -> !air.vertex ; fragment -> !air.fragment ; mesh -> !air.mesh
I. Vertex stage built-ins/metadata: air.vertex_id; output struct fields air.position + air.vertex_output;
   buffer args carry air.struct_type_info + air.buffer_size.
J. Fragment stage: [[stage_in]] -> air.position (with air.center + air.no_perspective) + air.fragment_input
   (air.center + air.perspective); output -> air.render_target (index 0).
K. Metal 4 MESH shader (air.mesh): mesh handle = %struct._mesh_t addrspace(7)*  (NEW address space 7 = mesh
   payload). Metadata air.mesh_type_info { vertex_type_info, primitive_type_info, max_verts, max_prims,
   air.triangle|air.line|air.point }. Intrinsics: air.set_vertex_*, air.set_primitive_data_*, air.set_index*,
   air.set_primitive_count_mesh. Template: mesh<V,P,NV,NP,topology>.
L. FULL ADDRESS-SPACE MAP (verified): 0=thread/private, 1=device, 2=constant/sampler, 3=threadgroup,
   7=mesh payload (Metal 4).
M. TO-DERIVE: object shader (api not exposed in this beta), ray tracing (metal_raytracing header present),
   Metal 4 ML command encoder (API-side MTL4MachineLearningCommandEncoder, NOT an MSL shader stage).

SCALAR PARAMS + STRUCT FIELDS (verified):
N. Kernel scalar inputs are NOT bare values — they must be `constant T&` and lower to `T addrspace(2)*
   dereferenceable(N)` (a constant-space pointer). Metadata: air.buffer, air.address_space=2, air.buffer_size=N,
   air.read. => BinC source `float dt` stays clean; the emitter auto-lowers it to a constant buffer + derefs on use.
O. Struct field access `p->f`: two-level GEP. In BinC's implicit element-wise model, `p->f` means `p[id].f`:
   `gep %struct.T, %p, i64 %id, i32 <fieldindex>` (field index 0-based, in declaration order).
