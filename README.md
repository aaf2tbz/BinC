# BinC

> **Works as C. Acts as Metal.**
> BinC is C, re-grounded on the Metal machine model — a genuinely new kind of language for the Apple GPU.

## Status: ✅ WORKING — compiles & runs on the GPU

A bootstrap compiler (in C) turns `.binc` source into real Metal binaries that execute correctly on Apple GPUs:

```
.binc  →  binc (compiler)  →  AIR (LLVM IR)  →  metal  →  .metallib  →  GPU dispatch  →  verified correct
```

Both example kernels run on the GPU with correct results (`proof/*.metallib`):
- `blend` — `out = a*0.5 + b*0.5`  ✅ all elements correct
- `step`  — struct fields + scalar param (`p->x += p->vx*dt`)  ✅ positions correct

## End goal: a BinC game of Pong

The destination is a **complete Pong game written in BinC** — where the C game of Pong is *actually Metal as well*: the **game logic** (ball, paddles, collision, score) runs on the GPU as compute kernels, and **the rendering** (vertex/fragment, or Metal 4 mesh shaders) draws the same scene on the GPU. The host is a thin layer — a window, a command queue, a dispatch. The whole game speaks Metal.

This is the fullest expression of *"works as C, acts as Metal"*: a real application whose **every line of game code is both familiar C and GPU-native**.

### Path to Pong
1. ✅ Substrate, paradigm, imperative core, divergence warning *(working now)*
2. ⏳ **Graphics stages** — vertex, fragment, mesh shaders *(AIR contract mapped in `RENDER.md`; compiler doesn't emit them yet)*
3. ⏳ Window + command loop in the host — thin Obj-C harness
4. ⏳ **Pong source** — ball + paddles + collision as a BinC compute kernel; the scene rendered by a BinC vertex/fragment kernel

## Quick start
```bash
cd binc && make verify      # build compiler + harness, compile all examples, run on GPU, verify
```
Or manually:
```bash
./binc ../examples/control.binc -o /tmp/control.metallib
./harness /tmp/control.metallib sc
```
Requires Xcode (provides `metal`/`metallib`); wrappers in `~/.local/bin`.

## Language features (working now)
- **Types:** `float`, `half`, `int`, `uint`, `bool`, and `struct`s of scalars
- **Pointers:** address-space-qualified — `device`, `constant`, `threadgroup`, `thread` (bare `T*` = `device`)
- **Scalar params** auto-lower to constant buffers (write plain `float dt`)
- **Locals:** `float x = ...;`, `int i = ...;` (alloca-backed, mutable across control flow)
- **Control flow:** `if` / `else if` / `else`, `for`, `while`, `return`
- **Operators:** `+ - * / %`, comparisons `== != < <= > >=`, logical `&& || !`, unary `-`, assignment `= += -= *= /= %=`
- **Implicit coercion:** int ↔ float on assignment
- **Implicit element-wise parallelism:** a function over a `device` buffer runs as a Metal grid (one thread per element)
- **Divergence awareness:** the compiler warns on data-dependent branches/loop bounds (the `TYPES.md` feature, live)

## The project
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
(`.metallib`). The substrate is real; the language design is unblocked.

## What BinC is (one paragraph)
Not a compiler for existing C, not CUDA, not a shading DSL. BinC is a language **defined on the GPU machine
model**, wearing C's syntax. A pointer *is* a device buffer; a function over a buffer *is* a parallel grid; the
element index is implicit in the semantics. You write C; it *is* Metal.

```c
struct Particle { float x, y, z; float vx, vy, vz; };
void step(struct Particle* p, float dt) {        // the entire program — a Metal compute grid
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    p->z += p->vz * dt;
}
```
