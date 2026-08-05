# BinC C-Family Facades

> *Works as C. Acts as Metal.* — Fourth design pillar.
> Delivers the founding promise: **C, C++, C#, and Objective-C — all in Metal.** Not four languages. **One.**

---

## The core idea: one language, four dialects

BinC has a **single canonical core** (its own clean syntax + GPU-native types + the typed parallelism/type
systems of `PARALLELISM.md` / `TYPES.md`). The four C-family languages are **surface dialects** — lexical
syntaxes that **desugar to the core** at parse time. One type system. One IR. One set of GPU rules. Four
familiar faces.

> A `class` (C++ facade), a `@interface` (Obj-C facade), and a `class` (C# facade) all desugar to the **same**
> `struct + vtable + methods`. A C# LINQ `Select`, a C++ `transform`, and a C `for` all desugar to the **same**
> BinC parallel `map`. Familiarity in, identical Metal out.

```
   .c  .cpp  .cs  .m  (facade dialects)        .binc (canonical core)
        │ facade parser (desugar)                   │
        └────────────────┬──────────────────────────┘
                         ▼
            BinC core AST  →  typed  →  AIR  →  .metallib   (ONE backend)
```

---

## What's CORE (identical across every facade)

These never change — they're BinC's GPU reality, from `TYPES.md` / `PARALLELISM.md`:
- Typed address spaces (`device`/`threadgroup`/`thread`/`constant`).
- Uniform / varying qualifiers (divergence as types).
- Domain-typed functions (implicit/explicit grids).
- `atomic<T>`, `sync()`, textures, render targets.
- **Value types + arenas, no GC. No host. No exceptions in kernels.**

A facade can change *how you spell* these, never *whether they exist*.

---

## The four facades

### 1. C facade — "near-verbatim C, runs on the GPU"
The closest to the core. Bare `T*` defaults to `device T*` (inferred). You write C; it's Metal.
```c
// c façade
struct Particle { float x,y,z, vx,vy,vz; };
void step(struct Particle* p, float dt) {        // p is device; grid inferred
    p->x += p->vx * dt;
}
```

### 2. C++ facade — classes, templates, namespaces
Adds the structural idioms the founders asked for, all desugaring to the core:
- `class`/`struct` + methods → `struct + vtable + fn(this,…)` (same lowering C++ itself uses).
- `template` → monomorphization (clone IR per type).
- `namespace`, references (`T&` → pointer), `using` → type alias.
```cpp
// c++ façade
namespace sim {
template<int N> struct Vec { float v[N]; };
class Particle { public: Vec<3> pos, vel;
    void step(float dt) { pos.v[0] += vel.v[0]*dt; /*…*/ } };
}
```

### 3. C# facade — properties, LINQ, delegates
C#-flavored surface; semantics stay GPU-native:
- `var`, properties, `using` → alias, delegates → function pointers.
- **LINQ → parallel kernels** (a beautiful fit): `Select`/`Where`/`Aggregate` desugar to BinC `map`/`filter`/
  `reduce` → the verified `air.kernel` / `air.atomic.*` paths.
- **No GC.** BinC is arena/value-type; C# reference types are opt-in and arena-scoped, not collected.
```csharp
// c# façade
device float[] Blend(device float[] a, device float[] b, float k) {
    return a.Select((x,i) => x*k + b[i]*k);        // -> parallel map -> air.kernel
}
```

### 4. Objective-C facade — @interface, message syntax, blocks
Obj-C-flavored surface; the *runtime* (message dispatch, `objc_msgSend`) stays host-side — only the compute
object model desugars:
- `@interface`/`@implementation` → `struct + methods`.
- `[obj msg:arg]` → method call (static, no runtime).
- Blocks `^{ … }` → kernel/function literals.
```objc
// objc façade
@interface Particle : NSObject - (void)step:(float)dt; @end
@implementation Particle
- (void)step:(float)dt { _x += _vx * dt; }   // desugars to struct + method fn
@end
```

---

## The same kernel, four dialects → one `.metallib`

```c
// C            : void step(struct Particle* p, float dt){ p->x += p->vx*dt; }
// C++          : void Particle::step(float dt){ pos.x += vel.x*dt; }
// C#           : particles.Step(dt);   // method, desugars to a grid map
// Obj-C        : [particle step:dt];
```
All four lower to the **same** AIR `air.kernel` with `device` pointer + `air.thread_position_in_grid`, link to
the **same** `.metallib`. That is "C, C#, C++, Obj-C in Metal" — literally.

---

## Why one-core/many-facades is the right call

1. **Shared everything that's hard.** Type system, divergence analysis, AIR lowering, host seam, optimization —
   built **once**, reused by all four. The facades are thin desugar passes.
2. **Community adoption.** A C#, C++, Obj-C, or C programmer sees familiar syntax day one; the GPU power is the
   same underneath. Lowest possible activation energy.
3. **No semantic explosion.** Because facades only change *surface*, BinC never has to implement four runtimes,
   four type systems, or four GCs. One GPU subset, clearly documented per facade.

---

## Honest limits

- **Each facade is a GPU subset, not the whole language.** C# reflection, `System.*`, LINQ-to-Objects over
  arbitrary host data; Obj-C runtime messaging; C++ exceptions/RTTI in kernels — out of scope or host-only.
  BinC documents the **device-safe subset** each facade supports.
- **GC is the hard line.** BinC does not and will not run a GC on the GPU. C#/Obj-C *reference* objects are
  arena-scoped value types in BinC. This is a real constraint, stated up front.
- **Desugar divergence is a maintenance cost.** Four parsers, kept in sync with one core grammar. Bounded, but
  real — the core grammar is the single source of truth they all target.

**Net:** BinC is one language that *looks* like the whole C family and *acts* like Metal — because the family
 resemblances are a translation layer over a single GPU-native core. The founding promise, made precise.
