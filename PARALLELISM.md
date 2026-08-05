# BinC parallelism

BinC has one execution model: every launchable function runs over a domain. The domain may be inferred or explicitly named.

---

## Implicit domains

A compute kernel with a device pointer can use the current element without spelling an index:

```c
kernel void add(device float* a, device float* b, device float* out) {
    *out = *a + *b;
}
```

The compiler supplies the hidden `air.thread_position_in_grid` value. `a`, `b`, and `out` are indexed by that value.

---

## Explicit domains

Use a coordinate parameter when the kernel needs 2-D/3-D coordinates, local positions, group positions, or explicit indexing:

```c
kernel void fill(device float* out, coord2D c) {
    int index = c.global.x + c.global.y * 64;
    out[index] = 1.0f;
}
```

Coordinate types lower as follows:

| BinC | AIR value | Built-in |
|---|---|---|
| `coord1D` | `i32` | `air.thread_position_in_grid` |
| `coord2D` | `<2 x i16>` | `air.thread_position_in_grid` |
| `coord3D` | `<3 x i16>` | `air.thread_position_in_grid` |
| `.local` | matching coordinate value | `air.thread_position_in_threadgroup` |
| `.group` | matching coordinate value | `air.threadgroup_position_in_grid` |
| `grid_extent` | `i32` | `air.threads_per_grid` |

A kernel has one coordinate domain parameter. A coordinate parameter makes the function a compute entry point even if `kernel` is omitted.

---

## Threadgroup memory

A threadgroup array is shared by the threads in a group:

```c
kernel void tile(device float* out, coord2D c,
                threadgroup float shared[16][16]) {
    shared[c.local.y][c.local.x] = 1.0f;
    sync();
    out[c.global.x] = shared[c.local.y][c.local.x];
}
```

The current compiler lowers these arrays to module-level `addrspace(3)` globals. The harness can dispatch 2-D grids with `grid2` and choose group dimensions with `group2`.

---

## Atomics

Atomic buffers use Metal's atomic wrapper representation:

```c
kernel void sum(device float* values,
               device atomic<float>* total,
               coord1D i,
               grid_extent n) {
    if (i < n) total->add(values[i]);
}
```

`atomic<float>::add` lowers to `air.atomic.global.add.f32`; integer add uses the integer intrinsic. Atomic buffers are marked read/write in AIR metadata.

---

## Synchronization and divergence

The compiler performs a small local uniformity analysis:

- coordinates are varying
- device-buffer data is varying
- parameters marked `varying` are varying
- constant/scalar parameters are uniform unless marked otherwise

A varying branch or loop bound gets a diagnostic. A `sync()` inside a varying region is rejected because a Metal barrier is undefined if some lanes skip it.

```c
kernel void unsafe(device float* p, coord1D i) {
    if (i % 2 == 0) {
        sync(); // compile error
    }
}
```

The current analysis is a safety gate, not a complete optimizer or full race checker.

---

## Harness dispatch

Specs support:

```text
grid 64
grid2 16 16
grid3 8 8 8
group 64
group2 8 8
group3 4 4 4
```

A one-dimensional `grid N` defaults to one threadgroup of N threads for compatibility with the original examples. Explicit 2-D/3-D specs use `dispatchThreads` with the requested group dimensions.

---

## Current limits

- There is no general texture coordinate type in the parser yet.
- Threadgroup race checking is not a complete dominance/data-flow system.
- Uniformity inference is intra-procedural.
- Nested parallelism and dynamic dispatch are not part of the current language.
