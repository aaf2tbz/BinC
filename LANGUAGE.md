# BinC language guide

BinC uses familiar C syntax, but its meaning is defined by the Apple GPU rather than by a CPU process. The central rule is:

> A function over device memory is a GPU operation over a grid.

There are two ways to name that grid:

1. **Implicit domain**: a device pointer uses the hidden thread position.
2. **Explicit domain**: a `coord1D`, `coord2D`, or `coord3D` parameter names the coordinate.

---

## A first kernel

```c
struct Particle { float x, y, vx, vy; };

kernel void step(device Particle* p, float dt) {
    p->x += p->vx * dt;
    p->y += p->vy * dt;
}
```

The source looks like ordinary C. In BinC, `p->x` means the `x` field of the current element. The compiler lowers it to a GEP using `air.thread_position_in_grid`.

A host dispatch still exists, but it is outside the BinC language. See `HOST.md`.

---

## Types

```c
float speed;
half packed_value;
uint counter;
float4 color;
```

Supported vector types are `float2`–`float4`, `int2`–`int4`, and `uint2`–`uint4`. Vectors support constructors, scalar splats, element-wise arithmetic, swizzle reads (`v.xy`, `v.wzyx`, `v.rg`) and swizzle writes (`v.xy = float2(...)`), plus single-component access such as `.x`, `.y`, `.z`, `.w`, `.r`, `.g`, `.b`, and `.a`.

Matrix types `mat2`, `mat3`, and `mat4` are column-major, with the same buffer layout as MSL matrices. Constructors take 1 scalar (diagonal), *N* column vectors, or *N*×*N* scalars in column-major order. Elements are addressed `m[col][row]`; `mat * vec`, `vec * mat`, `mat * mat`, `mat ± mat`, and `mat * scalar` are supported. Matrices can be locals, struct fields, and device/threadgroup pointers (matrix-by-value parameters and returns are not supported).

Module-level constants are declared `constant float PI = 3.14159f;` — one per line, scalar numeric types only. The initializer must be a literal.

Structs use C-like declarations:

```c
struct Body {
    float3 position;
    float mass;
    uint flags;
};
```

The compiler calculates MSL-compatible field offsets, size, and alignment for AIR metadata.

---

## Address spaces

Address spaces are part of the type:

```c
device float* global_values;
constant float* constants;
threadgroup float shared_values[256];
thread float private_value;
```

A bare pointer such as `float*` is treated as a device pointer. Address spaces lower to AIR address spaces 1, 2, 3, and 0 respectively.

---

## Implicit and explicit coordinates

The simple form uses a hidden 1-D coordinate:

```c
kernel void add(device float* a, device float* b, device float* out) {
    *out = *a + *b;
}
```

For 2-D work, name the domain:

```c
kernel void fill(device float* out, coord2D c) {
    int index = c.global.x + c.global.y * 64;
    out[index] = 1.0f;
}
```

Available coordinate properties:

- `.global`: position in the complete grid
- `.local`: position inside the threadgroup
- `.group`: threadgroup position in the grid

`grid_extent` maps to `air.threads_per_grid`:

```c
kernel void bounded(device float* out, coord1D i, grid_extent n) {
    if (i < n) out[i] = 1.0f;
}
```

---

## Shared memory and atomics

Threadgroup arrays become module-level AIR globals:

```c
kernel void tile(device float* out, coord2D c,
                threadgroup float tile[16][16]) {
    tile[c.local.y][c.local.x] = 1.0f;
    sync();
    out[c.global.x] = tile[c.local.y][c.local.x];
}
```

Atomic add uses a Metal atomic wrapper and AIR atomic intrinsic:

```c
kernel void sum(device float* values,
               device atomic<float>* total,
               coord1D i,
               grid_extent n) {
    if (i < n) total->add(values[i]);
}
```

A barrier in a varying branch is a compile error because not every thread is guaranteed to reach it.

---

## Control flow and functions

BinC supports:

```c
for (int i = 0; i < 10; i += 1) { }
while (condition) { }
if (condition) { } else { }
break;
continue;
return;
```

Plain functions can be called from kernels. Kernels cannot call other kernels, and recursion is rejected.

```c
void scale(device float* values, float k) {
    values[0] *= k;
}

kernel void run(device float* values, float k) {
    scale(values, k);
}
```

A plain function has no implicit thread index, so pointer accesses must be explicitly indexed (`values[0]`, `values[i]`, and so on).

---

## Operators

Integer types support the full bitwise set: `&`, `|`, `^`, `~`, `<<`, `>>` and their compound assignments, with `>>` arithmetic on `int` and logical on `uint`. Hex literals (`0xFF00`) are accepted. Explicit casts use C syntax: `(float)x`, `(uint)(-1)`, `(bool)x`, including scalar-to-vector splats (`(float4)1.0f`) and vector conversions. Casting a vector to a scalar is an error. Implicit conversions that can lose precision (float→int, and int→float except small constants) emit a compiler note — cast explicitly to silence.

## Built-ins

Scalar and vector (per-element) built-ins include:

- Math: `sqrt`, `fabs`, `floor`, `ceil`, `sin`, `cos`, `exp`, `log`, `fmin`, `fmax`, `pow`, `atan2`, `rsqrt`, `sign`
- Integer: `imin`, `imax`
- Vector math (scalars and vectors): `dot`, `cross` (float3), `length`, `distance`, `normalize`, `reflect`, `clamp`, `mix`, `step`, `smoothstep`, `fract`, `mod`, `radians`, `degrees`
- Selection: `select(a, b, mask)` picks per element (`mask ? a : b`) for scalar and vector operands
- Synchronization: `sync()`

Vector comparisons (`v1 < v2`, `==`, etc.) produce bool-vector masks for use with `select`.

## Textures and samplers

`texture2d<float>` (also `half`/`int`/`uint`) and `sampler` are supported kernel parameter types:

```c
kernel void k(device float* out, texture2d<float> tex, sampler smp, coord2D c) {
    float4 t = tex.read(int2(c.global.x, c.global.y));   // direct texel read
    float4 s = tex.sample(smp, float2(0.5f, 0.5f));      // filtered sample
    tex.write(float4(1.0f), int2(c.global.x, c.global.y)); // texel write
}
```

Textures use read-write access; reads/samples return a `float4` (half textures promote), and `write` takes a `float4` plus an `int2` coordinate. The harness binds textures declared with `tex <idx> <w> <h>` (deterministic fill: texel(x,y) = (x+1, y+1, x+y+1, 1)) and verifies kernel writes with `expecttex <idx> <r> <g> <b> <a>` host readback.

---

## Render stages

Vertex and fragment functions support the full stage contract:

```c
struct VOut { float4 pos [[position]]; float3 col; };
struct FIn  { float4 pos [[position]]; float3 col; };
struct FOut { float4 color0 [[color(0)]]; float4 color1 [[color(1)]]; float depth [[depth(any)]]; };

vertex VOut vs(device float4* vertices, vertex_id vid) {
    VOut o;
    o.pos = vertices[vid];
    o.col = float3(vertices[vid].xy, 1.0f);
    return o;
}

fragment FOut fs(FIn in) {
    FOut o;
    o.color0 = float4(in.col, 1.0f);
    o.depth = in.pos.z * 0.5f + 0.5f;
    return o;
}
```

Struct fields accept `[[position]]`, `[[flat]]`, `[[color(N)]]`, `[[depth(any)]]`, and `[[user(locnN)]]`. A vertex returning a struct emits `air.position` + `air.vertex_output user(locnN)` per field; a fragment taking a struct receives its fields as separate stage-in arguments (`air.fragment_input` with perspective or flat interpolation); a fragment returning a struct emits one `air.render_target` per `[[color(N)]]` field plus an `air.depth` output. The simple forms (`vertex float4`, `fragment float4`) remain supported. The harness verifies renders with `expectpix`/`expectdepth` pixel readback (see RENDER.md).

---

## What is not currently in the language

The following are future work, not silently supported features:

- General textures and samplers
- Arbitrary vertex-output structs and general `stage_in`
- Classes, methods, templates, generics, exceptions, and dynamic allocation
- C++, C#, and Objective-C source facades
- A complete type checker for every address-space conversion
- Full interprocedural uniformity and race analysis
