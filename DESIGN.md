# BinC design and feasibility

This document records what has been proven and what remains aspirational. It is intentionally separate from the implementation guide in `ARCHITECTURE.md`.

---

## The proven path

On the development machine, BinC emits textual AIR LLVM IR without using Clang to parse BinC source:

```text
BinC source → binc → AIR .ll → metal → .air → metallib → GPU
```

The compiler has produced real metallibs and dispatched them on the GPU. The current example suite covers scalar/vector/struct buffers, explicit domains, shared memory, barriers, atomics, render stages, and the Pong update kernel.

The strongest proof is not a pretty `.ll` file; it is `make verify`, which runs the produced metallibs through Metal hardware and checks outputs.

---

## Design principles

### GPU semantics first

BinC keeps C syntax where it helps readability, but its execution rules come from the GPU: grids, SIMT control flow, memory address spaces, vector values, barriers, and atomics.

### One source of truth

AIR metadata describes both the GPU entry point and the host binding. Generated headers use the same parsed parameter order and locations instead of inventing a parallel schema.

### Honest boundaries

BinC owns GPU work. Windowing, keyboard input, audio, drawable acquisition, and application lifecycle belong in the host seam.

### Small compiler, real behavior

The bootstrap compiler favors a direct implementation over a large dependency stack. The trade-off is a smaller language and less recovery/diagnostic infrastructure.

---

## Current architecture decisions

- The bootstrap compiler is C11 in `binc/src/`.
- AIR is emitted as text because Apple's `metal` driver validates and converts it to AIR bitcode.
- Kernels use implicit or explicit domains.
- Threadgroup arrays are module globals in address space 3.
- Atomic add is lowered directly to AIR atomic intrinsics.
- Vertex/fragment support starts with position-only stage I/O.
- The host seam is generated per metallib and backed by a thin Objective-C runtime.

---

## Current trade-offs

### AIR is versioned

The target triple and metadata follow the installed Xcode beta. A future Xcode update may require new probes and emitter changes.

### The compiler is intentionally incomplete

There is no production optimizer, full type checker, general texture language, class system, generic system, or facade parser yet. Those are not hidden dependencies; they are named future work.

### The Pong host is intentionally not BinC

A GPU language cannot create an `NSWindow` or receive keyboard events by itself. Pong keeps the host small and keeps all game behavior/render geometry on the GPU side.

---

## Next sensible milestones

1. Improve compiler diagnostics and source locations.
2. Add proper stage-output structs and interpolants.
3. Add texture/sampler types and texture read/write intrinsics.
4. Replace hand-numbered metadata with a structured metadata builder.
5. Add automated probes for each Xcode/AIR version.
6. Explore facade parsers only after the core language/type rules stabilize.
