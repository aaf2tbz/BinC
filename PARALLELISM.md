# BinC Parallelism Model

> *Works as C. Acts as Metal.* — First design pillar.
> Grounded in **verified** AIR metadata (see `reference/air_*.ll`). Every construct below has a real lowering.

---

## One rule, two spellings

> **Every BinC function executes over a *domain* — a set of coordinates. The domain is part of the function's
> type. It is either inferred (implicit) or named (explicit).**

- **Implicit:** no coordinate parameter → the domain is *element-wise over the device buffer(s)* (1 thread per
  element, 1D). The `step` example. The index is elided.
- **Explicit:** a `coord<N>` / `grid` parameter *names* the domain → full control: 2D/3D, threadgroups, shared
  memory, reductions, stencils.

These are **not two modes**. They are one model — a domain-typed function — at two levels of spelling. Implicit
is sugar that infers the domain; explicit is naming it. You upgrade by adding a coordinate parameter.

---

## The vocabulary (BinC type → verified AIR lowering)

| BinC | AIR / Metal | Verified |
|---|---|---|
| `device T*` | `T addrspace(1)*` + `"air-buffer-no-alias"` | ✅ |
| `threadgroup T` (shared mem) | module global in `addrspace(3)` | ✅ |
| `thread T` (private/register) | `addrspace(0)` | ✅ |
| `constant T*` (read-only) | `addrspace(2)` | ✅ (sampler path) |
| `atomic<T>` | struct `metal::_atomic { T }` + `air.atomic.global.*` | ✅ |
| `texture2d<T,access>` | `%struct._texture_2d_t addrspace(1)*` + `!air.texture` | ✅ |
| `coord<N>` | `<N x i16>` (ushort2/3) | ✅ |
| `.global` / `.local` / `.group` | `air.thread_position_in_grid` / `..._in_threadgroup` / `air.threadgroup_position_in_grid` | ✅ |
| `sync()` | `threadgroup_barrier(mem_threadgroup)` | ✅ |
| `grid_extent` | `air.threads_per_grid` | ✅ |

**Address-space map (verified):** `0`=thread, `1`=device, `2`=constant/sampler, `3`=threadgroup.

---

## The spectrum (BinC source → what it lowers to)

### 1. Implicit element-wise (the `step` example)
```c
struct Particle { float x,y,z; float vx,vy,vz; };
void step(device Particle* p, float dt) {       // domain INFERRED: 1 thread per particle
    p->x += p->vx * dt;
}
```
→ domain = 1D over `p`; implicit index `i` from `air.thread_position_in_grid` (i32); `p->x` = `p[i].x`.

### 2. Stencil (explicit 1D — needs neighbors)
```c
void smooth(device const float* in, device float* out, coord1D i) {
    out[i] = 0.25f*in[i-1] + 0.5f*in[i] + 0.25f*in[i+1];   // neighbor access needs the index
}
```
→ same 1D grid as #1, but `i` is now a named `coord1D`; bounds handled by the domain (BinC guarantees `i` is
in-range; neighbor access is the programmer's responsibility or a typed boundary mode).

### 3. Tiled 2D with shared memory + barrier
```c
void tileblur(texture2d<float4> src, texture2d<float4, write> dst,
              coord2D c, threadgroup float4 smem[16][16]) {
    smem[c.local] = src[c.global];        // cooperative load into addrspace(3)
    sync();                               // threadgroup_barrier
    dst[c.global] = smem[c.local];
}
```
→ `c` is `<2 x i16>`; `c.global`/`c.local` map to the two built-ins; `smem` is an `addrspace(3)` module global;
`sync()` is `threadgroup_barrier`. Threadgroup size (16×16) is a dispatch attribute.

### 4. Reduction (atomic accumulator)
```c
void sum(device const float* in, device atomic<float>* acc, coord1D i, grid_extent n) {
    if (i < n) acc->add(in[i]);           // -> air.atomic.global.add.f32
}
```
→ `acc` is a `metal::_atomic` struct buffer; `.add()` lowers to `air.atomic.global.add.f32`; `n` is
`air.threads_per_grid`.

---

## Reconciliation: why implicit and explicit are the same thing

A BinC function's **domain** is computed by a single rule:

1. If the signature contains a `coord<N>` or `grid` parameter → **that is the domain** (explicit).
2. Else, if it has a `device T*` parameter → **element-wise domain over that buffer** (implicit): BinC synthesizes
   a hidden 1D `coord` named `i`, and rewrites `p->f` / `p[i]` into `p[i].f` / `p[i]`.
3. Else → not a kernel (compile error: no parallelism domain).

So `step` is exactly `smooth`/`sum` with the coordinate elided. There is **one** model; implicit is the ergonomic
90%-case, explicit is the escape hatch for the hard 10%. Both are first-class BinC, both lower to real AIR.

---

## Proven vs. to-derive

**Proven (compiled to `.ll`, contract captured in `reference/`):** 1D/2D/3D grids, device/threadgroup/thread/
constant address spaces, textures (read/write), threadgroup shared memory + barriers, atomics/reductions,
`threads_per_grid`. → The entire **compute parallelism** surface is mapped.

**To-derive next (compile MSL probes → read `.ll`):** threadgroup size attributes, vertex/fragment render path,
Metal 4 mesh shaders & machine-learning command encoder, ray tracing, indirect command buffers. Same method;
mechanical.

---

## Open sub-questions (to sharpen next)

1. **Threadgroup size:** declared in the type (`grid2D<tile<16,16>>`) or set at launch? Likely *both* — a default
   in the type, overridable at dispatch.
2. **Boundary handling for stencils:** typed boundary modes (`edge`, `clamp`, `zero`) vs. programmer-managed?
3. **Reduction ergonomics:** is `sum` the spelled-out form, or does BinC offer a higher-level `reduce(+, buf)`?
4. **Nested parallelism:** grids of grids (a kernel launching subgrids) — does Metal 4's compute encoder support
   this cleanly?
