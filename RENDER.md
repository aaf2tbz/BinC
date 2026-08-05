# BinC Render Path & Metal 4 — The AIR Stage Map

> *Works as C. Acts as Metal.* — Target surface for graphics + Metal 4.
> **Every entry below is verified** by compiling real MSL to `.ll` on this machine (`reference/air_*.ll`).

---

## The unifying fact: stage is metadata, not syntax

In AIR, a function's **stage** is a *named module metadata node*, not a keyword inside the function:

| BinC stage | AIR metadata | Proven file |
|---|---|---|
| compute kernel | `!air.kernel` | `example_air64_kernel.ll` |
| vertex | `!air.vertex` | `air_render_vertex_fragment.ll` |
| fragment | `!air.fragment` | `air_render_vertex_fragment.ll` |
| **mesh (Metal 4)** | `!air.mesh` | `air_metal4_mesh_shader.ll` |

This is *perfect* for BinC: a BinC function is just an LLVM function; its "stage" is one metadata line. BinC's
typed stages (`kernel`/`vertex`/`fragment`/`mesh`) lower to the matching `!air.*` node. No magic.

---

## MSL standards

`metal3.0 / 3.1 / 4.0 / 4.1`. The toolchain **defaults to Metal 4.1** (`!"Metal", 4, 1, 0`). BinC targets the
latest; the contract is stable across them.

---

## Address spaces — the complete verified map

| AS | Meaning | BinC type | Verified in |
|---|---|---|---|
| 0 | thread / private (registers) | `thread T` | every probe |
| 1 | device | `device T*` | every probe |
| 2 | constant / sampler | `constant T*` | mvp uniform, `air.get_read_sampler()` |
| 3 | threadgroup (shared) | `threadgroup T` | `air_tiled2d_*` |
| **7** | **mesh payload (Metal 4)** | *(internal)* | `air_metal4_mesh_shader.ll` |

---

## Render path (vertex + fragment)

**Vertex stage** — input from buffers/built-ins, output a struct with `[[position]]`:
- built-in `air.vertex_id`; output fields tagged `air.position` + `air.vertex_output`.
- buffer args carry `air.struct_type_info` (struct layout) and `air.buffer_size`.

**Fragment stage** — consume the rasterized struct via `[[stage_in]]`:
- `air.position` (interpolation `air.center`, `air.no_perspective`),
- `air.fragment_input` (interpolation `air.center`, `air.perspective`),
- output → `air.render_target` (index 0).

```c
// BinC render (illustrative lowering targets above)
struct VOut { float4 pos; float4 color; };
vertex VOut vs(uniform uint vid, device const VIn* v, constant float4x4* mvp) {
    VOut o; o.pos = mul(*mvp, v[vid].pos); o.color = float4(1,0,0,1); return o;
}
fragment float4 fs(stagein<VOut> in) { return in.color; }
```

---

## Metal 4 — mesh shaders (programmable primitive pipeline)

The mesh stage replaces vertex/geometry assembly: a threadgroup **emits** vertices + primitives directly.

- **Stage:** `!air.mesh`.
- **Handle:** `%struct._mesh_t addrspace(7)*` — a new address space (7 = mesh payload).
- **Type metadata:** `air.mesh_type_info { vertex_type_info, primitive_type_info, NV, NP, air.triangle|line|point }`.
- **Emit intrinsics:** `air.set_vertex_*`, `air.set_primitive_data_*`, `air.set_index*`, `air.set_primitive_count_mesh`.
- **MSL template:** `mesh<Vertex, Primitive, NV, NP, topology>`.

```c
// BinC mesh stage (illustrative) — emits one triangle, no vertex buffer
mesh<void, VertexOut, PrimOut, 3, 1, topology::triangle>
void emit_triangle(coord3D gid) {
    set_vertex(0, {float4(-1,-1,0,1)});
    set_vertex(1, {float4( 1,-1,0,1)});
    set_vertex(2, {float4( 0, 1,0,1)});
    set_primitive_count(1);
}
```

---

## Coverage & what's left

**Verified & mapped (BinC can target these today):** compute kernels (1D/2D/3D, buffers, textures, threadgroup,
barriers, atomics/reductions), full render pipeline (vertex + fragment + stage_in + render targets), and the
Metal 4 mesh shader. That's the **entire modern Metal compute + graphics surface.**

**To-derive (same method — compile MSL → read `.ll`):**
- **Object shader** — the mesh-pipeline amplification stage; its API isn't exposed in this beta (`object_payload`
  / `mesh_grid` aren't standalone types yet; only `mesh_grid_properties` exists).
- **Ray tracing** — `metal_raytracing` header is present; intersection/bound-volume stages to be probed.
- **Metal 4 ML command encoder** — *not* an MSL shader stage; it's an API-side `MTL4MachineLearningCommandEncoder`
  that runs Core ML models on the GPU timeline. BinC's ML story is via the compute stage + model loading, not a
  new shader type.

**Net:** BinC's target surface is now the full proven Metal compute + render + Metal-4-mesh contract. The
language design (parallelism + types) maps cleanly onto all of it.
