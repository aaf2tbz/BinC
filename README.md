# BinC

> **Works as C. Acts as Metal.**
> BinC is C, re-grounded on the Metal machine model — a genuinely new kind of language for the Apple GPU.

## What BinC is

Not a compiler for existing C, not CUDA, not a shading DSL. BinC is a language **defined on the GPU machine
model**, wearing C's syntax. A pointer *is* a device buffer; a function over a buffer *is* a parallel grid; the
element index is implicit in the semantics. You write C; it *is* Metal.

```c
struct Particle { float x, y, z; float vx, vy, vz; };

kernel void step(device Particle* p, float dt) {   // the entire program — a Metal compute grid
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    p->z += p->vz * dt;
}
```

C was designed as a portable layer over the PDP-11's machine model — "high-level assembly for the CPU."
**BinC is high-level assembly for the GPU.** It keeps C's syntax and mental model (low-friction, close to the
machine, you can read it like C), but the *machine* it abstracts is Apple Silicon's GPU via Metal. Its
primitives — SIMT execution, address spaces, the memory hierarchy — are defined on that model, not annotated
onto a CPU language.

## How it operates

BinC has its own compiler, written from scratch in C (~1000 lines). It does not use Clang, and BinC source
never becomes `.metal`/MSL. The compiler emits **AIR** (Apple's GPU LLVM IR) directly, as text:

```
.binc ──▶ binc ──▶ AIR (.ll) ──▶ metal ──▶ .air ──▶ metallib ──▶ .metallib ──▶ GPU
          your compiler          (Apple's AIR assembler/linker — the shared backend every Metal language uses)
```

The language's semantics are implemented in the compiler, not borrowed from a frontend:

| You write | It *means* (and lowers to) |
|---|---|
| `device Particle* p` | a Metal device buffer — `%struct.Particle addrspace(1)*` + `!air.buffer` metadata |
| `kernel void step(...)` | a Metal compute grid — one thread per element, entry in `!air.kernel` |
| `p->x` / `*p` | `p[id].x` / `p[id]` — a coalesced device access at the implicit `air.thread_position_in_grid` |
| `float dt` (scalar param) | a constant buffer — `float addrspace(2)* dereferenceable(4)`, auto-dereferenced on use |
| `device` / `constant` / `threadgroup` / `thread` | **types**, not attributes — AIR address spaces 1 / 2 / 3 / 0 |
| `float4`, `struct` | native AIR vectors and laid-out struct types (MSL layout rules) |
| `sqrt`, `fmin`, `sync()`, … | AIR intrinsics — `air.fast_sqrt.f32`, `air.fast_fmin.f32`, `air.wg.barrier`, … |

Address spaces and divergence live in the **type system**, so the compiler proves GPU properties statically:
data-dependent branches and loop bounds are flagged at compile time (live today), and the design
(`TYPES.md`) extends this to barrier-in-divergent-flow and shared-memory race prevention as hard errors.

The full function-by-function map of the compiler, every BinC→AIR mapping, and the metadata contract:
**[`ARCHITECTURE.md`](ARCHITECTURE.md)**. The end-to-end playable sample is `examples/pong.binc` plus
`examples/pong_host.m`; run `cd binc && make pong`, then `./pong_host` (W/S or A/Z moves the left paddle).

## Status: ✅ WORKING — compiles & runs on the GPU

Every example in `examples/` compiles to a real `.metallib` and is **verified executing correctly on the GPU**
by a data-driven harness (`make verify` — the suite covers scalar/vector/struct buffers, explicit 1D/2D
coordinates, threadgroup memory, barriers, atomic reduction, control flow, calls, builtins, and half/uint precision).

## Quick start
```bash
cd binc && make verify      # build compiler + harness, compile all examples, run each on the GPU, verify
```
Or manually:
```bash
./binc ../examples/control.binc -o /tmp/control.metallib      # compile
./harness /tmp/control.metallib ../examples/control.spec      # dispatch on GPU + check results
```
Requires Xcode (provides `metal`/`metallib`); wrappers in `~/.local/bin`. Spec format is documented at the
top of `binc/harness.m`.

## Language features (working now)
- **Types:** `float`, `half` (real 16-bit f16, auto-promoted in expressions), `int`, `uint` (true unsigned ops/predicates), `bool`, and `struct`s of mixed scalar/vector fields
- **Vectors:** `float2/3/4`, `int2/3/4`, `uint2/3/4` — constructors `float4(...)` (incl. single-scalar splat), element-wise arithmetic with vector/scalar operands, `.x/.y/.z/.w` (+`.r/.g/.b/.a`) component read/write, vector buffer elements and struct fields
- **Pointers:** address-space-qualified — `device`, `constant`, `threadgroup`, `thread` (bare `T*` = `device`); subscript `p[i]`, pointer arithmetic `*(p + k)`, field chains `p->f` / `p[i].f` / `s.f`
- **Explicit domains:** `coord1D/2D/3D` parameters map to AIR thread-position built-ins; `.global`, `.local`, and `.group` expose grid coordinates; `grid_extent` maps to `air.threads_per_grid`
- **Parallel memory:** `threadgroup T name[N][M]` lowers to an addrspace(3) module global; `atomic<float>`/`atomic<int>` buffers support `.add()` through AIR atomics
- **Uniformity gate:** coordinate and varying data make control flow divergent; `sync()` in divergent control flow is a compile error
- **Scalar params** auto-lower to constant buffers (write plain `float dt`)
- **Locals:** scalar, vector, and struct locals (alloca-backed, mutable across control flow)
- **Control flow:** `if` / `else if` / `else`, `for`, `while`, `break`, `continue`, `return` (with a value in non-kernel functions)
- **Functions:** `kernel void` entry points plus plain internal functions with real return types — callable from kernels and each other (no recursion; kernels are not callable)
- **Builtins:** `sqrt fabs floor ceil sin cos exp log fmin fmax pow` (float), `imin imax` (int), `sync()` (threadgroup barrier)
- **Operators:** `+ - * / %`, comparisons `== != < <= > >=`, logical `&& || !`, unary `-`, assignment `= += -= *= /= %=`
- **Implicit coercion:** int ↔ uint ↔ float on assignment
- **Implicit element-wise parallelism:** a function over a `device` buffer runs as a Metal grid (one thread per element)
- **Divergence awareness:** the compiler warns on data-dependent branches/loop bounds and rejects barriers in divergent control flow
- **Render stages:** `vertex` + `fragment` stage keywords emit external AIR entry points with position/render-target metadata; `vertex_id` is a typed built-in
- **Host seam v1:** every compiled metallib gets a sibling generated binding header; `binc_runtime.h/.m` provides device, buffer, dispatch, and completion primitives

## The project
- **[`ARCHITECTURE.md`](ARCHITECTURE.md)** — how the compiler works: every function, mapping, and metadata node.
- **[`LANGUAGE.md`](LANGUAGE.md)** — the paradigm. *What BinC is.* (Read this first.)
- **[`PARALLELISM.md`](PARALLELISM.md)** — pillar 1: the unified implicit+explicit grid model.
- **[`TYPES.md`](TYPES.md)** — pillar 2: address spaces & divergence as types (the differentiator).
- **[`HOST.md`](HOST.md)** — pillar 3: the host seam — how BinC launches & talks to the CPU (reflection-generated).
- **[`FACADES.md`](FACADES.md)** — pillar 4: C/C++/C#/Obj-C as one core, four dialects (the founding promise).
- **[`RENDER.md`](RENDER.md)** — the full AIR stage map: compute + render + Metal 4 mesh (all verified).
- **[`DESIGN.md`](DESIGN.md)** — technical feasibility, **proven** on this machine.
- **[`proof/blend_vision_proof.ll`](proof/blend_vision_proof.ll)** — hand-authored BinC IR → valid `.metallib`.
- **[`reference/`](reference)** — reverse-engineered AIR contract (compute/render/mesh) + verified facts.

## What's proven
A from-scratch language emitting LLVM IR (no `.metal`, no Clang) links to a real Metal executable
(`.metallib`) and runs correctly on the GPU. The substrate is real, and the compute core of the language —
types, vectors, structs, control flow, functions, builtins — is implemented and GPU-verified.
