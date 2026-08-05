# BinC Type System — Address Spaces & Divergence as Types

> *Works as C. Acts as Metal.* — Second design pillar (the differentiator).
> **No GPU language makes these first-class types. BinC does.** The compiler then *proves* GPU
> correctness/performance properties statically instead of hoping.

Two independent axes, both in the type system:

| Axis | What it captures | Lowers to AIR? |
|---|---|---|
| **Address-space-qualified pointers** | memory hierarchy + lifetime + access cost | ✅ yes (`addrspace(N)`) |
| **Uniform / varying qualifiers** | thread divergence (SIMT lockstep) | ❌ no — pure compile-time gate |

---

## Axis 1 — Address spaces are types, not attributes

In MSL/CUDA, `device`/`__shared__` are *attributes* on a pointer. In BinC, the address space is *part of the
type*, so they are **distinct nominal types** that the type checker treats as incompatible unless you go through
a defined, costed operation.

### The types
```
ptr<device,      T>    device memory  — addrspace(1), app-lifetime, coalesced
ptr<constant,    T>    read-only      — addrspace(2), broadcast/cached
ptr<threadgroup, T>    shared memory  — addrspace(3), threadgroup-lifetime, banked
ptr<thread,      T>    private/reg    — addrspace(0), per-thread, fastest
```
`device Particle* p` in the earlier examples is sugar for `ptr<device, Particle> p`.

### The conversion algebra (each is a *defined operation*, never an implicit cast)

| From → To | Operation | Cost / rule |
|---|---|---|
| `device → thread` | **load** one element into a register | allowed; coalesced if index uniform-ish |
| `constant → thread` | broadcast read | allowed; cheapest device read |
| `threadgroup → thread` | load | allowed **only after `sync()`** if a prior write occurred |
| `thread → device` / `thread → threadgroup` | **store** | allowed |
| `device → threadgroup` | **cooperative tiled load** (a typed op, not a cast) | requires a `coord` + tile shape |
| `device → device` | pointer arithmetic (stencil neighbor) | allowed; the pointer type is unchanged |

### What this buys (static guarantees nobody else makes)
- **No accidental cross-hierarchy assignment.** `d = s;` (device ← threadgroup) is a *type error*, not a silent
  miscompile.
- **Lifetime in the type.** `threadgroup` values can't escape the threadgroup; the borrow checker ties their
  validity to the threadgroup's scope.
- **Sync-correctness baked into loads.** A `threadgroup → thread` load that reads a value written by another
  thread in the same group is a *type error unless a `sync()` dominates the read*. **BinC prevents data races on
  shared memory at compile time** — a whole class of GPU bug gone.

```c
void bad(device float* d, threadgroup float* s, coord1D c) {
    s[c.local] = 1.0f;                 // write shared
    float v = s[c.local];              // ERROR: read of threadgroup value without sync() after write
    sync();
    float w = s[c.local];              // OK
}
```

---

## Axis 2 — Uniform / varying (divergence as a type)

SIMT threads execute in lockstep within a group. **Divergence** — threads taking different paths — serializes
execution and is the #1 performance footgun. It is also the source of *correctness* bugs (a barrier under a
divergent branch is undefined behavior). **BinC makes uniformity a type.**

### The qualifiers
```
uniform  T   // identical across all threads in the group   (e.g. grid extent, loop bound)
varying  T   // may differ per thread                        (e.g. the thread coordinate)
```
Default inference rules (so programmers rarely annotate):
- `coord<N>` and `air.thread_position_in_grid/_threadgroup` → **varying**.
- `air.threads_per_grid`, `constant` values, `threadgroup_position` shape → **uniform**.
- `uniform ⊕ uniform → uniform`; **anything touching `varying → varying`**.
- A buffer *pointer* is `uniform` (shared base address); indexing it by a `varying` index yields `varying` data.

### The killer static guarantee: barriers must be in uniform control flow
A `sync()` (threadgroup barrier) is **undefined behavior in Metal if any thread in the group doesn't reach it.**
BinC makes this a **compile-time error**:

```c
void bad(device float* d, coord1D i) {
    if (i % 2 == 0) {     // i is varying  ->  branch is DIVERGENT
        sync();           // ERROR: barrier in divergent control flow (UB on the GPU)
    }
}
```
This eliminates, at compile time, one of the most common and hardest GPU bugs to reproduce. **No current GPU
language gives you this.**

### Uniform loop bounds enforced
Per-thread loops (varying bounds) are a *different, expensive* execution model. BinC flags them:

```c
void sum(device float* in, device atomic<float>* acc, coord1D i) {
    for (int k = 0; k < i; k++) { ... }   // ERROR: bound 'i' is varying; needs a 'uniform' bound
}
```

---

## How the two axes interact (example)

```c
void blur(device const float* in, device float* out,
          coord1D i, uniform int radius) {     // radius is uniform -> shared by all threads
    float acc = 0.0f;                          // thread-local register (varying data)
    for (int k = -radius; k <= radius; k++) {  // bound is uniform -> lockstep loop (cheap)
        acc += in[i + k];                      // device -> thread load (coalesced-ish)
    }
    out[i] = acc / (2*radius + 1);             // thread -> device store
}
```
The type system *knows*: `acc` is varying & thread-local; the loop is uniform (free); the loads are
device→register. The compiler can emit optimal code and *prove* there is no barrier/divergence bug — because
none can exist in accepted BinC.

---

## Honesty: what's tractable vs research-grade

- **Tractable (BinC v1):**
  - Address spaces as nominal types + the conversion algebra. → maps 1:1 to AIR `addrspace(N)`. **Proven.**
  - Dominance-based check that `sync()` is in uniform control flow and that threadgroup read-after-write has an
    intervening `sync()`. → standard data-flow analysis.
  - **Intra-procedural** uniformity inference (within one kernel). → bounded, decidable.
- **Research-grade (later):**
  - **Inter-procedural / whole-program** uniformity inference (across calls, generics). Hard; may require
    annotations on the function boundary.
  - **Coalescing as a type** (static guarantee of coalesced vs scattered access). Genuinely hard (depends on
    runtime indices); likely a *best-effort* cost model, not a hard type.
- **What it costs the programmer:** almost nothing for v1 — divergence/address-space are inferred locally;
    annotations (`uniform`, `varying`) appear mainly on function parameters and when the programmer wants to
    *force* a guarantee.

---

## Why this is the real differentiator

Every other GPU language (MSL, CUDA, Sycl, HIP) treats address space and divergence as *runtime/analysis*
concerns the compiler *guesses* at. BinC promotes them to the **type system**, so:

1. Whole classes of GPU bug (data races on shared memory, barriers in divergent flow) are **compile errors**.
2. Performance footguns (varying loop bounds, divergent branches) are **flagged**.
3. The compiler can reason about and *optimize* with guarantees it currently can only assume.

This is what "acts as Metal" means at the deepest level: the language's types *are* the GPU's machine reality.
