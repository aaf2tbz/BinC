# BinC architecture

This document describes the compiler that exists today. It is deliberately shorter and more operational than a design manifesto: start here when changing the implementation.

---

## 1. End-to-end pipeline

```text
.binc source
   │
   ├─ lexer.c       source text → tokens
   ├─ parser.c      tokens → Program AST
   ├─ codegen.c     AST → textual AIR LLVM IR
   └─ main.c        AIR → metal → AIR bitcode → metallib

.metallib + generated .h
   │
   └─ binc_runtime.m or harness.m → Metal dispatch on the GPU
```

The compiler is a bootstrap C11 program. It has no external parser library and does not invoke Clang to understand BinC source. Apple's `metal` tool is used as the AIR validator/link input because the installed Apple toolchain owns the AIR target.

---

## 2. Source files

| File | Responsibility |
|---|---|
| `binc/src/binc.h` | Token kinds, types, AST structures, public compiler entry points |
| `binc/src/lexer.c` | C-like tokenizer with line/column tracking, comments, literals, keywords |
| `binc/src/parser.c` | Recursive-descent parser (structs, functions, expressions, statements), per-construct error recovery, `constant` globals, `const` locals, texture/sampler/matrix types |
| `binc/src/codegen.c` | Type-directed AIR emission (~2,100 lines): values, memory, control flow, uniformity, built-ins, composite math, matrices, texture intrinsics, structured metadata builder |
| `binc/src/main.c` | File loading, CLI, compiler driver, `xcrun`-based `metal`/`metallib`, generated host header |
| `binc/harness.m` | Generic spec parser (buffers, textures, samplers), Metal dispatch, result comparison, texture readback |
| `binc/binc_runtime.h/.m` | Thin host runtime used by generated headers |

The implementation intentionally uses short-lived process memory. AST nodes and strings are allocated with `malloc`/`realloc` and are reclaimed by process exit.

---

## 3. Type representation

`Type` records:

- scalar kind (`float`, `half`, `int`, `uint`, `bool`, struct)
- vector width
- pointer/address space
- coordinate dimensionality
- atomic payload kind
- threadgroup array dimensions

`TypeKind` also includes compiler-only domain types such as `T_COORD`, `T_GRID_EXTENT`, and `T_ATOMIC`.

Runtime expression values use `ValKind` to preserve float, signed integer, unsigned integer, boolean, pointer, and vector-width behavior while generating LLVM operations.

---

## 4. Parsing model

The parser is a conventional recursive-descent parser:

1. Parse a type and optional address-space qualifier.
2. Parse a function, struct, parameter, or statement.
3. Parse expressions by precedence (`assignment` → logical → comparison → arithmetic → postfix).
4. Store source constructs in a compact AST.

Important syntax decisions:

- `kernel` marks a compute entry point. A coordinate parameter also makes a function a compute entry point.
- `vertex` and `fragment` make external render entry points.
- `vertex_id` is a thread-space unsigned built-in parameter.
- Threadgroup arrays are parameter declarations that become module-level address-space-3 globals.
- Atomic methods such as `acc->add(value)` are represented as method-shaped call expressions.

The lexer accepts both `//` and `/* ... */` comments.

---

## 5. Code generation

### Values and memory

The emitter creates typed LLVM pointers and uses the actual pointee type for every GEP, load, and store. This is important for mixed structs, vectors, half values, atomics, and threadgroup arrays.

- Implicit kernel pointer access uses the hidden `%_id` widened to i64.
- Explicit coordinate indexing uses the named coordinate value.
- Scalar kernel parameters are lowered to constant address-space buffers and cached after their first load.
- Plain-function scalar parameters remain by-value.
- Half values are promoted to float for arithmetic and truncated on stores.
- Vector components use `extractelement` for reads and typed component pointers for writes.

### Control flow

`if`, `while`, and `for` become numbered basic blocks. `break` and `continue` use a small loop-label stack. A `term` flag prevents instructions being emitted after a terminating branch or return.

### Uniformity and divergence

The current analysis is intentionally intra-procedural. It identifies coordinate values, explicitly varying parameters, and device-buffer data as varying. It warns about varying branches/loop bounds and tracks a divergent-region depth. Calling `sync()` while that depth is nonzero calls `die()` with a compile error.

### Built-ins

Math calls map to AIR fast math intrinsics. `sync()` maps to `air.wg.barrier`. Atomic add calls map to `air.atomic.global.add.f32` or `.i32`.

### Render stages

Vertex and fragment functions are emitted as externally visible functions. Metadata currently supports:

- vertex position output
- vertex buffers and `vertex_id`
- fragment position input
- fragment float4 render target 0

The compiler intentionally does not claim to support arbitrary stage I/O structs yet.

---

## 6. AIR module layout

Each generated module contains:

1. AIR target datalayout and `air64_v29` triple
2. Struct declarations
3. Threadgroup globals and atomic wrapper declarations when used
4. Function definitions
5. Built-in and atomic declarations
6. Attribute groups
7. `!llvm.module.flags`, compile options, version, language, and source metadata
8. `!air.kernel`, `!air.vertex`, and/or `!air.fragment` entry metadata
9. Argument metadata with resource locations, access modes, sizes, alignments, and names

Metadata numbering is generated manually in `codegen.c`. When changing metadata, inspect the emitted `.ll` and run `make verify` immediately.

---

## 7. Host seam

After linking the metallib, `main.c` emits a sibling binding header. The header provides one inline wrapper per compute kernel. Pointer arguments become `BincBuffer*`; scalar arguments become C scalar values; coordinate and grid built-ins stay dispatch-owned.

`binc_runtime.m` provides:

- device and metallib loading
- shared buffer allocation
- buffer contents/native handle access
- typed argument records
- compute dispatch and completion waiting
- runtime access to Metal device, queue, and library for custom render hosts

The Pong host uses the runtime for its compute update and then uses the exposed Metal objects to encode the BinC vertex/fragment render pipeline.

---

## 8. Verification

`make verify`:

1. builds `binc` and the Objective-C harness
2. compiles every `examples/*.binc` (28 examples: compute, bitwise, casts, ternary, control flow, swizzles, vector select, matrices, textures)
3. dispatches every matching `.spec` on the GPU
4. compares integer words exactly and float words with tolerance; textures declared with `tex <idx> <w> <h>` are filled deterministically and kernel writes verified by host readback (`expecttex`)
5. reports `ALL EXAMPLES VERIFIED ON GPU`

Additional gates: `make test-negative` (compile-error suite), `make test-fuzz` (mutation fuzzer; has caught two real bugs — a layout-sensitive varargs crash and a NULL struct-metadata deref), and `.github/workflows/ci.yml` (GPU-free: build + negative + fuzz + compile every example to `.metallib`). The Pong spec checks a safe GPU update dispatch with the complete state and vertex-buffer sizes. The playable host is additionally built by `make pong` and smoke-launched with `nohup` during development.

---

## 9. Known limits

- AIR metadata and target versions follow the installed Xcode beta.
- The compiler is a bootstrap compiler, not a production optimizer.
- The current type checker does not implement every planned address-space/race guarantee.
- General stage-I/O, mesh, class, generic, and facade support remains future work even when reference AIR probes exist for those concepts.
