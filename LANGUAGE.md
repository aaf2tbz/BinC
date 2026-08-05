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

Supported vector types are `float2`–`float4`, `int2`–`int4`, and `uint2`–`uint4`. Vectors support constructors, scalar splats, element-wise arithmetic, and single-component access such as `.x`, `.y`, `.z`, `.w`, `.r`, `.g`, `.b`, and `.a`.

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

## Built-ins

Current scalar built-ins include:

- Math: `sqrt`, `fabs`, `floor`, `ceil`, `sin`, `cos`, `exp`, `log`, `fmin`, `fmax`, `pow`
- Integer: `imin`, `imax`
- Synchronization: `sync()`

---

## Render stages

The current render surface is intentionally minimal:

```c
vertex float4 vs(device float4* vertices, vertex_id vid) {
    return vertices[vid];
}

fragment float4 fs(float4 pos) {
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
```

Vertex output is position-only. A fragment can receive the rasterized position and return a float4 render target. The Pong example uses this to create depth-based neon color and atmospheric effects.

---

## What is not currently in the language

The following are future work, not silently supported features:

- General textures and samplers
- Arbitrary vertex-output structs and general `stage_in`
- Classes, methods, templates, generics, exceptions, and dynamic allocation
- C++, C#, and Objective-C source facades
- A complete type checker for every address-space conversion
- Full interprocedural uniformity and race analysis
