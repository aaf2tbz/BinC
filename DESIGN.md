# BinC — A New Language for Metal 4

> **Status:** feasibility study, **vision proven against the real toolchain on this machine.**
> Machine: Xcode 27 beta 4 · `metal` front-end 32023.921 · AIR v29 · MTL4 core API.

---

## 0. What BinC IS (per the founder's vision)

BinC is a **new programming language, built from the ground up**, that provides constructs analogous to
**C, C#, C++, and Objective-C** (functions, structs, classes, methods, generics) — but re-implemented
**natively in LLVM IR** and compiled to **Metal binaries (`.metallib`)** for the GPU.

- It is **not** a clang wrapper. It is **not** a source-to-source translator.
- BinC has **its own syntax, type system, and IR emitter.**
- "Written in LLVM bitcode" means: BinC's semantics (its structs/classes/methods/dispatch) lower to, and
  its runtime is authored as, **LLVM IR** that conforms to Apple's **AIR** contract → links to a `.metallib`.

---

## 1. ✅ THE VISION IS PROVEN (ground truth)

The make-or-break question was: *can LLVM IR that was NOT produced by the `metal` front-end become a valid
`.metallib`?* **Answer: YES.** Proof artifact: `proof/blend_vision_proof.ll`.

I hand-authored a kernel `binc_blend` with `!llvm.ident = "BinC compiler v0.0.1"` (no `.metal`, no
`metal_stdlib`, no Clang) and compiled it through:

```
blend.ll  →(metal front-end)→  blend.air  →(metallib / AIR-LLD)→  blend.metallib
                                                                  MetalLib executable v1.2.9
```

### The AIR contract BinC's emitter must satisfy (reverse-engineered, complete for buffers):

| Element | Required value | Source |
|---|---|---|
| `target triple` | `"air64_v29-apple-macosx27.0.0"` (version tracks toolchain) | observed |
| `target datalayout` | the long fixed string (see `reference/example_air64_kernel.ll`) | observed |
| Device-buffer pointers | `T addrspace(1)*` + param attr `"air-buffer-no-alias"` | observed |
| Address spaces | `0`=thread, `1`=device, `2`=constant, `3`=threadgroup | MSL spec |
| Entry point | `!air.kernel = !{ <fn ptr>, !<empty>, !<arg-list> }` | observed |
| Per-arg binding | `!{ i32 <idx>, !"air.buffer", !"air.location_index", i32 <bind>, i32 1, !"air.read"\|"air.read_write"\|"air.write", !"air.address_space", i32 <as>, !"air.arg_type_size", i32 N, ... }` | observed |
| Built-in args | e.g. `!"air.thread_position_in_grid"` | observed |
| Resource limits | `!air.max_device_buffers 31`, `..._constant_buffers 31`, `..._textures 128`, `..._samplers 16` | observed |
| Identity | `!air.version = !{ i32 2, i32 9, i32 0 }`, `!air.language_version = !{ !"Metal", i32 4, i32 1, i32 0 }` | observed |

**Toolchain behavior:** `metal foo.ll` runs the IR back through the Clang/Metal front-end and re-validates
the AIR contract (it is not a bare assembler). Therefore BinC must emit a **complete, consistent** AIR module.
A faithful reproduction links cleanly; an incomplete one causes a front-end crash (not a clean reject) — so
BinC's emitter must be disciplined. (No `llvm-as`/`llvm-dis` ships here; `metal -emit-llvm -S` is the IR path.)

### Other verified facts (see `reference/VERIFIED_FACTS.md`)
- `.air` = LLVM bitcode (`0xBC 0xC0DE` magic) inside an Apple wrapper.
- `metal` front-end is a patched Clang for target `air64`.
- Generic host-target clang bitcode is **rejected** by `metallib`.
- `metal-ir-converter` is **gone** (deprecated; AIR-LLD replaced it).
- **Metal 4 (WWDC25) = new `MTL4*` core API, NOT a new shading language.** MSL unchanged ⇒ our AIR target is stable.

---

## 2. Honest scope: what "C/C++/C#/Obj-C in Metal" realistically means on a GPU

A GPU has no CPU object model. "Classes and methods" therefore lower exactly as they do in C++: a class is a
`struct` + a vtable pointer + free functions taking `this` as the first arg. BinC implements that lowering in
IR. This is well-trodden (C++, Rust, Swift all do it).

| Language construct | BinC→IR difficulty | Notes |
|---|---|---|
| Functions | 🟢 trivial | `define` |
| `struct` / value types | 🟢 trivial | LLVM struct types |
| Arrays / pointers | 🟢 easy | must carry address space |
| **Generics / templates** (monomorphization) | 🟡 medium | clone IR per type-instantiation |
| **Classes + methods + vtables** | 🟡 medium | struct + fn-ptr table; covers C++/C#/Obj-C methods |
| **Operators / overload resolution** | 🟡 medium | standard frontend work |
| C# **delegates / events** | 🔴 hard | fn-ptr + capture; feasible for compute |
| C# **GC-managed objects** on GPU | 🔴 very hard | GPUs dislike GC; arena/ownership model instead |
| **Obj-C message dispatch** (`objc_msgSend`) | 🔴 hard | host-runtime; compute subset only |
| **Reflection / `System.*`** | 🔴 out of scope for kernels | CPU-only |

**Realistic BinC v1 surface:** value types, functions, classes-with-methods, generics, operator overloading,
address-space-aware pointers, compute kernels + (later) vertex/fragment. This is the *shared core* of all four
languages and is a coherent, shippable language. C#/Obj-C *runtime* object models are flagged out-of-kernel.

---

## 3. Architecture (from-scratch language)

```
  BinC source (.binc)            ; own syntax, C-family-inspired
        │
        ▼
  ┌─────────────────────────────────────┐
  │  binc front-end (Rust)              │
  │   lexer → parser → AST →            │
  │   type-check + borrow/address-space │
  │   inference → monomorphize          │
  └──────────────┬──────────────────────┘
                 ▼
  ┌─────────────────────────────────────┐
  │  IR emitter  →  textual LLVM IR (.ll) │   ; conforms to AIR contract (§1)
  │   structs, fns, vtables, dispatch     │
  │   + !air.kernel / !air.* metadata     │
  └──────────────┬──────────────────────┘
                 ▼
        metal  →  .air  →  metallib  →  .metallib
                 ▼
  ┌─────────────────────────────────────┐
  │  binc runtime (C, linked in app)    │
  │   load .metallib, bind MTL4 arg     │
  │   tables, reflection from metadata  │
  └─────────────────────────────────────┘
```

**Why emit textual `.ll` (not build IR via the LLVM C-API)?** It decouples BinC from any specific LLVM build
version, keeps the compiler a pure-Rust project, and the proven path already works (`metal blend.ll`).
Trade-off: BinC must itself emit valid LLVM IR text (a real but bounded task; the grammar is stable).

---

## 4. Phased roadmap (from-scratch)

### Phase 0 — Done ✅
Vision proven. AIR contract reverse-engineered & saved in `reference/` and `proof/`.

### Phase 1 — Minimal language + one codegen target (≈ 6–10 weeks)
- Define a tiny **`.binc` syntax** for: value types, functions, `device`/`thread` pointer kinds, and a `kernel` modifier.
- Lexer/parser/type-checker in Rust.
- IR emitter that reproduces `proof/blend_vision_proof.ll` from BinC source.
- **Milestone:** `binc build blend.binc → blend.metallib`, dispatched + verified by a C/Swift harness vs CPU.

### Phase 2 — Classes, methods, generics (≈ 3–5 months)
- Struct + vtable lowering; method calls → `fn(this, ...)`.
- Monomorphizing generics.
- Struct/array buffers in AIR (nested `!air.*` arg metadata — to be reverse-engineered next).
- **Milestone:** a small linear-algebra / particle subsystem written in idiomatic BinC.

### Phase 3 — The four-language facades (≈ ongoing)
- **C facade**: BinC dialect that accepts near-C syntax (pointers, structs, `malloc`-free arenas).
- **C++ facade**: classes/templates map to BinC classes/generics.
- **Obj-C facade**: compute subset; message-send stays CPU-side via a boundary annotation.
- **C# facade**: BinC subset + an IL→BinC bridge (Cecil) for compute kernels (no `System.*` host deps).

### Phase 4 — The optimization layer (the "elevation")
- Kernel fusion / megakernels, address-space propagation, cross-boundary specialization, profile-guided layout.
- These operate on BinC's own IR/AST before emission — an advantage a clang-wrapper could never have.

---

## 5. Honest risks

1. **AIR is closed & versioned** (`air64_v29` now). The contract can change between toolchain releases.
   **Mitigation:** pin toolchain; keep `reference/example_air64_kernel.ll` as a regression oracle BinC re-derives
   its emitter from; CI-compile a probe on every Xcode update.
2. **Front-end re-validation** (`metal foo.ll` re-runs Clang) means incomplete/inconsistent IR **crashes** rather
   than errors cleanly. BinC's emitter must be conservative and self-check against the oracle.
3. **`metal -emit-llvm -S`** is our only IR-introspection tool here (no `llvm-as`/`llvm-dis` shipped). For richer
   AIR features (textures, render targets) we reverse-engineer by compiling MSL examples and reading their `.ll`.
4. **GPU ≠ CPU.** Recursion, virtual dispatch, and dynamic allocation are costly/forbidden in kernels. BinC must
   statically enforce a "device-safe" subset and clearly diagnose violations.
5. **Scale.** A from-scratch, four-language, optimizing compiler is a multi-year program. Phase 1 is the proof
   that it's worth it — and Phase 0 already says yes.

---

## 6. Recommended first concrete milestone

A **minimal BinC** that compiles this source to the proven `blend.metallib`:

```c
// blend.binc  — Phase 1 target syntax (illustrative)
device ptr<readonly  float> a,
device ptr<readonly  float> b,
device ptr<writeonly float> out  // kernel args

kernel void blend(a, b, out, thread_id id) {
    let i = id;
    out[i] = a[i] * 0.5 + b[i] * 0.5;
}
```

…emitting `proof/blend_vision_proof.ll` verbatim → `.metallib`. That closes the loop from *BinC source* to
*Metal binary*, and everything else scales from there.
