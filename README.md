# BinC

> **Works as C. Acts as Metal.**

BinC is a small, GPU-native language with C-like syntax. It compiles `.binc` source directly to Apple's AIR LLVM IR, then uses Apple's `metal` and `metallib` tools to produce a real `.metallib` executable. It does not translate BinC to MSL and does not use Clang as a BinC frontend.

The project now includes a complete playable 3-D Pong example. The game simulation, physics, rounds, lives, scoreboard state, arena geometry, particles, motion blur, vertex shader, and atmospheric fragment shader are written in BinC. The host is a thin Objective-C window/input/audio/Metal seam.

---

## Current status

**Working and GPU-verified on this machine:**

- Scalar, integer, unsigned, half, vector, struct, and matrix (`mat2`/`mat3`/`mat4`) values
- Device, constant, thread, and threadgroup address spaces
- Implicit 1-D element-wise kernels
- Explicit `coord1D`, `coord2D`, and `coord3D` domains
- Grid extent and threadgroup coordinate built-ins
- Threadgroup shared-memory arrays and `sync()` barriers
- Atomic add operations
- Divergence warnings and compile-time rejection of barriers in divergent control flow
- `texture2d<float|half|int|uint>` and `sampler` kernel parameters with `read()`/`write()`/`sample()` methods
- Bitwise operators, shifts, hex literals, explicit casts, and lossy-conversion warnings
- Ternary expressions, `do-while` loops, `switch` statements, module `constant` globals
- Vector swizzle reads (`v.wzyx`) and writes (`v.xy = ...`), vector comparisons, and `select`
- Full vectorized math library (`dot`, `cross`, `length`, `normalize`, `clamp`, `mix`, `step`, `smoothstep`, `fract`, `mod`, `atan2`, `rsqrt`, `sign`, ...)
- `const` locals with write rejection, pointer-argument type/address-space checks
- Pointer arithmetic, indexing, field chains, locals, loops, calls, and built-ins
- AIR vertex and fragment entry points with position/render-target metadata
- Generated host binding headers and a small Metal runtime shim
- A playable 3-D Pong game with audio and keyboard input

`make verify` compiles and dispatches every example on the GPU (compute, textures, and Pong smoke spec included). `make test-negative` runs the compile-error suite; `make test-fuzz` runs the mutation fuzzer; `.github/workflows/ci.yml` runs the GPU-free pipeline (build + negative tests + fuzz + compile-every-example) on push.

---

## Quick start

Requirements:

- macOS with an Apple GPU
- Xcode beta toolchain configured in `binc/Makefile`
- `metal`, `metallib`, and `xcrun`

Verify the compiler and all examples:

```bash
cd ~/binc/binc
make verify
```

Build the playable game:

```bash
cd ~/binc/binc
make pong
./pong_host
```

Run it in the background for testing:

```bash
cd ~/binc/binc
nohup ./pong_host >pong_host.log 2>&1 < /dev/null &
```

Controls:

- **Up Arrow** or **W**: move up
- **Down Arrow** or **D**: move down
- **Escape**: quit
- Use the on-screen restart button after a win or loss

---

## The compiler pipeline

```text
program.binc
    │
    ├─ binc lexer/parser/code generator
    │
    ▼
program.ll       textual AIR LLVM IR
    │
    ├─ metal program.ll -c
    ▼
program.air
    │
    ├─ metallib program.air -o program.metallib
    ▼
program.metallib  GPU executable
```

When a metallib is produced, `binc` also writes a sibling host binding header. For example, compiling `pong.binc` to `build/pong.metallib` creates `build/pong.h`.

---

## A small example

```c
struct Particle { float x, y, vx, vy; };

kernel void step(device Particle* p, float dt) {
    p->x += p->vx * dt;
    p->y += p->vy * dt;
}
```

With no explicit coordinate parameter, `p->field` uses the hidden thread position in the grid. An explicit domain is available when the kernel needs named coordinates:

```c
kernel void paint(device float* out, coord2D c) {
    int i = c.global.x + c.global.y * 64;
    out[i] = 1.0f;
}
```

---

## Language features

### Values and layouts

- `float`, `half`, `int`, `uint`, `bool`
- `float2`–`float4`, `int2`–`int4`, `uint2`–`uint4`
- C-like structs with mixed scalar/vector fields
- Struct locals and field assignment
- MSL-compatible vector and struct layout metadata

### Memory and parallelism

- `device T*`, `constant T*`, `threadgroup T`, and `thread T`
- Bare `T*` defaults to `device T*`
- `coord1D`, `coord2D`, `coord3D`
- `coord.global`, `coord.local`, and `coord.group`
- `grid_extent`
- `threadgroup T name[N]` and `name[N][M]`
- `atomic<float>` and `atomic<int>` with `acc->add(value)`

### Expressions and control flow

- Arithmetic, comparisons, logical operators, assignment, and compound assignment
- Bitwise operators (`&`, `|`, `^`, `~`, `<<`, `>>`) with compound forms and hex literals
- Explicit casts `(float)x`, `(uint)(-1)`, including scalar-to-vector splats
- Ternary `cond ? a : b`, `do-while` loops, and `switch` with fallthrough
- `const` locals (writes are compile errors)
- Pointer indexing and arithmetic
- `if`, `for`, `while`, `break`, `continue`, and returns
- User functions without recursion
- Built-ins including `sqrt`, `sin`, `cos`, `pow`, `atan2`, `rsqrt`, `sign`, `fmin`, `fmax`, `imin`, `imax`, and `sync()`
- Vector constructors, arithmetic, component access, and swizzle reads/writes
- Vector comparisons producing mask vectors, plus `select(a, b, mask)`
- Vector math: `dot`, `cross`, `length`, `distance`, `normalize`, `reflect`, `clamp`, `mix`, `step`, `smoothstep`, `fract`, `mod`, `radians`, `degrees`
- `mat2`/`mat3`/`mat4`: column-major constructors, `m[col][row]` indexing, `mat*vec`, `mat*mat`, scalar arithmetic
- Module-level `constant` globals

### Textures

```c
kernel void k(device float* out, texture2d<float> tex, sampler smp, coord2D c) {
    float4 t = tex.read(int2(c.global.x, c.global.y));
    float4 s = tex.sample(smp, float2(0.5f, 0.5f));
    tex.write(float4(1.0f), int2(c.global.x, c.global.y));
}
```

`texture2d` element types are `float`, `half`, `int`, and `uint` (read-write access). Reads/samples return a `float4` (half promotes); `write` takes a `float4` and an `int2` coordinate.

### Render stages

```c
struct VOut { float4 pos [[position]]; float3 col; };
struct FOut { float4 color0 [[color(0)]]; float4 color1 [[color(1)]]; float depth [[depth(any)]]; };

vertex VOut vs(device float4* vertices, vertex_id vid) { ... }
fragment FOut fs(VIn in) { ... }
```

Stage structs, `[[position]]`/`[[flat]]`/`[[color(N)]]`/`[[depth(any)]]` attributes, interpolants, multiple render targets, and depth output are supported; the pixel-verifying harness checks renders with `expectpix`/`expectdepth`.

---

## Pong

`examples/pong.binc` contains the GPU game. It includes:

- 3-D depth-layered arena geometry
- Physics-aware paddle impacts
- A ball that preserves upward/downward impact direction
- Smooth alpha-blended motion ribbons rather than solid echo blocks
- Particle fields around the ball and paddles
- 25%-faster rounds with a 50%-faster starting speed than the original game
- A 30-second in-game timer rendered at the top of the playfield
- Lives, scores, round progression, AI misses, win/loss states, and restart behavior
- A different atmospheric theme every five rounds, including a star-field theme

`examples/pong_host.m` owns only the platform seam: window creation, keyboard events, audio playback, drawable/depth setup, and render command encoding. The game state and all GPU geometry are BinC buffers and kernels.

Audio assets:

- `pong_music.wav`: a full multi-section soundtrack whose intensity and playback position change between rounds
- `pong_hit.wav`: contact sound
- `pong_life.wav`: life-loss sound
- `pong_score.wav`: score/round sound

---

## Repository map

| Path | Purpose |
|---|---|
| `binc/src/` | Bootstrap compiler source in C11 |
| `binc/harness.m` | Spec-driven Metal GPU verification harness |
| `binc/binc_runtime.h/.m` | Host runtime used by generated bindings |
| `examples/` | BinC programs and matching `.spec` files |
| `examples/pong_host.m` | Playable Pong host application |
| `reference/` | AIR files and verified toolchain facts |
| `proof/` | Early hand-authored AIR proof |
| `ARCHITECTURE.md` | How the current compiler works |
| `LANGUAGE.md` | BinC language guide |
| `PARALLELISM.md` | Domains, coordinates, shared memory, and atomics |
| `TYPES.md` | Address spaces and divergence rules |
| `HOST.md` | Generated host seam and runtime |
| `RENDER.md` | Vertex/fragment support and current limits |

---

## Honest limitations

BinC is a working bootstrap compiler, not a finished production language. The current implementation does not yet provide arbitrary stage-I/O structs, arrays of device buffers, generics, exceptions, dynamic allocation, or the C++/C#/Objective-C facades described as future design work. Those ideas are documented separately as plans, not current features.

The compiler targets the installed Apple AIR contract. AIR is versioned, so changing Xcode versions may require updating the target triple, metadata, or probes in `reference/`.
