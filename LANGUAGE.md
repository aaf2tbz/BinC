# BinC — A New Kind of Language for Metal

> **Tagline:** *Works as C. Acts as Metal.*
> **One-line definition:** BinC is C, re-grounded on the Metal machine model.

---

## The Idea

C was designed as a portable layer over the PDP-11's machine model — "high-level assembly for the CPU."
**BinC is high-level assembly for the GPU.** It wears C's syntax and philosophy (low-friction, close to the
machine, you can read it like C), but the *machine* it abstracts is Apple Silicon's GPU via Metal.

It is **not**:
- a compiler for existing C (that's a bridge, not a language),
- C with GPU extensions (that's CUDA),
- a shading DSL (that's MSL/HLSL/GLSL),
- single-source host+device C++ (that's Sycl/oneAPI).

It **is**: a language whose primitives are *defined* on the GPU machine model — SIMT execution, address
spaces, the memory hierarchy, vector widths — wearing C's clothes.

> **The naming pun:** *BinC* = binary C — C that becomes a Metal **binary**; C that sits at the **binary**/
> machine layer of the GPU. And "Metal" — close to the metal.

---

## What "works as C" means

- Familiar C syntax: blocks, braces, types, `struct`, pointers, functions, `for`.
- The mental model a C programmer already carries. You *read* BinC like C.
- No exotic keywords or ceremony (`[[buffer(0)]]`, `<<<>>>`, `__global__`).

## What "acts as Metal" means

- Execution is **SIMT**. A function over a collection is, by definition, a parallel grid.
- **Address spaces are types**, not attributes: a pointer *is* `device` / `constant` / `threadgroup` / `thread`.
- The **memory hierarchy** is first-class (register → threadgroup → device → constant).
- Divergence, coalescing, and bank conflicts are things the language and type system *know about*.
- There is no `kernel` keyword, because in BinC **everything is the GPU.** There is no host in the core.

---

## The central design move

> **A pointer is a device buffer. A function over a buffer is a parallel map. The element index is implicit
> in the language's semantics.**

```c
struct Particle { float x, y, z; float vx, vy, vz; };

void step(struct Particle* p, float dt) {     // this is the entire program
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    p->z += p->vz * dt;
}
```

Call `step(particles, dt)` and BinC launches a Metal compute grid — one thread per particle. The `p->`
dereference is a coalesced device load/store. No annotations, no launch syntax. The C you already know how to
write *becomes* Metal, because BinC's semantics are *defined* on the GPU.

---

## Why it's genuinely new (the honest comparison)

| | Host/Device split | GPU-ness lives in | Ergonomics |
|---|---|---|---|
| **MSL / HLSL** | embedded DSL, GPU-only | attributes (`[[buffer(0)]]`, `thread_position_in_grid`) | ceremony-heavy |
| **CUDA** | C++ extended (`__global__`, `<<<>>>`) | annotations + launch syntax | C++ with a GPU mode |
| **Sycl / oneAPI** | single-source C++ | abstractions over host+device | generic C++ |
| **BinC** | **GPU is the only machine** | **the type system & semantics** | **plain C** |

The novelty is twofold:
1. **No host/device schizophrenia.** BinC is defined *on* the GPU. (A thin runtime/dispatch boundary exists
   only where a program must talk to the CPU — see "Honest limits" below.)
2. **GPU reality is typed, not attributed.** Address space, coalescing, and layout are *types*. The compiler
   can **prove** GPU correctness/performance properties statically — something no current GPU language does.

---

## Honest limits (the truth about "C on a GPU")

- **No host in the core** ⇒ BinC programs need a *boundary* to the CPU (launch, I/O, windowing). That boundary
  is a small, explicit runtime seam — not part of BinC-the-language. (Think: the C `main` is replaced by a
  `dispatch` declaration; the rest is pure GPU.)
- **C idioms that don't fit a GPU are out of the core language** (or redefined): unbounded recursion, `malloc`
  at runtime, `FILE*` I/O, `setjmp`/`longjmp`. BinC offers GPU-native alternatives (iteration over recursion,
  fixed arenas, no I/O in kernels).
- **Determinism over magic.** BinC is *not* "auto-parallelize arbitrary C." Its constructs are *defined* as
  parallel/Metal operations, so behavior is always knowable — the compiler doesn't guess, it guarantees.

---

## Substrate (proven feasible — see `DESIGN.md`)

BinC compiles to **LLVM IR conforming to Apple's AIR contract → `.metallib`** Metal executables. The vision is
already *proven*: a hand-authored IR module (`!llvm.ident = "BinC compiler v0.0.1"`, no `.metal`, no Clang)
links to a valid `.metallib` on this machine. So the language design is unblocked — the substrate is real.

---

## Open design questions (to develop next)

1. **The element-implicit model.** Is "a function over a buffer = a per-element grid" the *only* parallel form,
   or do we also allow explicit grids/2D/3D dispatch as typed constructs?
2. **Address-space typing.** Concrete type syntax for `device`/`constant`/`threadgroup`/`thread` pointers — and
   how conversion/promotion between them works.
3. **The host seam.** What does the launch/dispatch boundary look like? A `dispatch` declaration? A separate
   host language? Or stay pure-GPU and define a wire protocol?
4. **Divergence & coalescing as types.** How far do we push static GPU-correctness? (This is the moonshot that
   no GPU language does and the real differentiator.)
5. **C-family facade depth.** How much of C/C++/C#/Obj-C *syntax* do we honor vs. define our own clean core?
