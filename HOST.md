# BinC Host Seam — How BinC Reaches the CPU

> *Works as C. Acts as Metal.* — Third (final) design pillar.
> The one pillar that makes BinC a **usable language**, not a kernel-only spec.

---

## The philosophy: the host is *not* BinC

BinC-the-language is **pure GPU** (no host in the core). So the host seam is **not a host language inside BinC.**
It is a **reflection-generated contract**: a BinC program *declares* its launchable interface, and the compiler
emits (1) the `.metallib` and (2) a **host binding library** + thin runtime that drives the Metal 4 API. The
host stays whatever language the app already is (Swift / Obj-C / C++).

> **The unifying move:** the same `!air.*` argument metadata that *describes a GPU function* also *describes how
> the host binds to it.* One source of truth — proven metadata, not a parallel schema.

```
   BinC source (.binc)
        │ binc compiler
        ├─▶ program.metallib              (GPU code; entry points + !air.* arg metadata)
        └─▶ program.{h,swift,mm}          (host binding: typed dispatch + buffer handles)
                  │ uses
                  └─▶ binc_runtime        (thin C lib wrapping MTL4: device, queue, arg tables, sync)
```

---

## Host seam v1 (implemented)

`binc` now emits a sibling `<output>.h` binding header whenever it produces a metallib. The header contains a
small typed inline wrapper for every launchable BinC kernel. Pointer parameters become `BincBuffer *` handles,
scalar parameters retain their C scalar type, and coordinate/grid built-ins are supplied by the dispatch domain
rather than exposed as fake host arguments.

`binc_runtime.h` and `binc_runtime.m` provide the deliberately thin implementation: open a metallib, allocate
shared buffers, bind buffer/constant arguments at their reflected locations, dispatch, wait for completion, and
expose shared contents. The runtime is compiled with `make runtime` and intentionally does not invent a second
binding schema; generated wrappers use the same parameter order and locations as the AIR metadata. The Pong host
adds only Cocoa window/input, AVFoundation looping music, drawable/depth setup, and render-command encoding;
arrow-up/W move up and arrow-down/D move down.

## The dispatch declaration

A BinC program declares its **launchable surface** explicitly. The functions are still BinC (domain-typed); the
`entry` block fixes their dispatch contract:

```c
struct Particle { float x,y,z; float vx,vy,vz; };
void step(device Particle* p, uniform float dt) {       // a BinC kernel (implicit 1D grid)
    p->x += p->vx * dt;  p->y += p->vy * dt;  p->z += p->vz * dt;
}

program Sim {                                            // the host seam — a declaration, not host code
    device buffer<Particle> particles;                   // host-provided, app-lifetime
    uniform float dt;

    entry step(particles, dt) grid = particles.length;   // domain + binding
    // graphics stages add: render_target, depth, drawables — same declaration form
}
```

The compiler turns `entry step(...) grid = particles.length` into:
- a Metal 4 **compute dispatch** (grid = N threads, N = buffer extent),
- a host function `Sim.step(buffer, dt)` that builds the `MTL4ArgumentTable`, encodes, and `commit:count:`.

---

## Buffer lifecycle — typed ownership

| BinC declaration | Metal 4 backing | Who owns | Sync |
|---|---|---|---|
| `device buffer<T>` | shared/managed `MTLBuffer` | host allocates, both read/write | fence on handoff |
| `device private buffer<T>` | private `MTLBuffer` (GPU-only) | BinC allocates, GPU-only | none (faster) |
| `constant buffer<T>` | read-only, broadcast | host, immutable per dispatch | none |
| `texture2d<T>` | `MTLTexture` | host (drawable) or BinC | fence / drawable |

Ownership is **typed**, so the host seam never leaks a GPU-only buffer to the CPU or vice-versa — it's a type
error, consistent with `TYPES.md`.

---

## CPU↔GPU synchronization as part of the contract

Metal 4 uses **untracked resources + explicit barriers** (verified). BinC extends its compile-time sync story
(`TYPES.md`'s `sync()`-dominance check) **across the boundary**: a dispatch's completion is an explicit
`fence`/`event` in the declaration, so the host knows *exactly* when results are valid — no implicit guarantees
to get wrong.

```c
entry step(particles, dt) grid = particles.length
    outputs { particles }                              // host may read after this fence
    fence = cpu_readable;                              // explicit handoff
```

---

## Why this design is coherent with the rest of BinC

1. **One source of truth.** `entry` bindings mirror the `!air.*` arg metadata exactly (verified for buffers,
   textures, render targets, structs). Generating the host binding is reading BinC's own emitted metadata.
2. **Types flow through.** A `device buffer<T>` on the BinC side is a typed `MTLBuffer*` handle on the host side;
   the host can't mis-bind it (wrong type / wrong stage) — it's checked at the seam.
3. **Multi-threaded by construction.** Metal 4's `MTL4CommandAllocator` enables parallel command encoding; BinC
   dispatches are independent entries, so the runtime can encode many `Sim.step(...)` across threads for free.
4. **No host language invented.** BinC stays pure-GPU; the host is the app's existing language. BinC just makes
   itself **drivable** with zero boilerplate.

---

## Honesty: limits & open questions

- **The host is still required.** BinC does not eliminate the CPU; it minimizes the seam to a typed, generated
  contract. A real app still has a `main`/event loop — just not in BinC.
- **Windowing/drawables.** Render/mesh stages need a host-provided `CAMetalLayer` drawable; BinC declares the
  `render_target` binding, the host supplies the surface.
- **Open questions to sharpen:**
  1. Is `program { entry ... }` the only seam spelling, or do we allow per-kernel `export` annotations too?
  2. Async/streaming dispatch: first-class `stream` types for pipelined host↔GPU, or explicit queues?
  3. Reflection API: does BinC ship a runtime *reflection* lib (so a host can bind dynamically), or only
     compile-time generated bindings?

**Net:** with the host seam, BinC is a complete language — pure-GPU core, typed parallelism, compile-time GPU
correctness, and a reflection-generated, type-checked bridge to any host. Every piece is grounded in verified
Metal/Metal-4 fact.
