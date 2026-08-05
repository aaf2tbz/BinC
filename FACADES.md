# C-family facades: future direction

BinC has one implemented core language: `.binc` source with C-like syntax and GPU-native semantics. C, C++, C#, and Objective-C facades are a design direction, not current compiler inputs.

The goal is a shared pipeline:

```text
C / C++ / C# / Objective-C surface syntax
                    │
                    ▼
             BinC core AST
                    │
                    ▼
            AIR and metallib
```

A facade should change spelling, not GPU behavior. Every facade would share:

- address-space types
- implicit and explicit domains
- uniformity/divergence rules
- threadgroup memory and atomics
- the same AIR emitter
- the same host seam

---

## Possible mappings

### C

The current `.binc` grammar is already closest to a restricted C dialect: structs, pointers, functions, loops, and expressions.

### C++

Future syntax could desugar classes and methods into structs plus functions whose first argument is `this`. Templates could become monomorphized BinC functions.

### C#

Future syntax could provide a value-type/device-safe subset. Garbage collection, reflection, exceptions, and ordinary .NET runtime services cannot run inside a GPU kernel.

### Objective-C

Future syntax could map static methods and a restricted object model to structs/functions. Cocoa and Objective-C runtime messaging remain host-side, as demonstrated by `examples/pong_host.m`.

---

## Explicit non-goals for the current release

- accepting `.c`, `.cpp`, `.cs`, or `.m` as compiler input
- implementing four separate runtimes
- running a garbage collector on the GPU
- pretending that host object models work inside kernels

The facade idea remains valuable because one GPU-native type system and one AIR backend can serve many familiar frontends later.
