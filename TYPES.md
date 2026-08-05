# BinC types: memory and uniformity

BinC treats GPU memory placement and SIMT uniformity as language concepts. Some parts are fully implemented today; others are deliberately documented as the next type-system layer rather than claimed as finished.

---

## Address spaces

| BinC spelling | AIR address space | Meaning |
|---|---:|---|
| `thread T` | 0 | private per-thread storage |
| `device T*` | 1 | application-visible GPU memory |
| `constant T*` | 2 | read-only/broadcast memory |
| `threadgroup T` | 3 | shared memory for a threadgroup |

A bare pointer defaults to `device`. Address-space information is preserved in pointer signatures, GEPs, loads, stores, and AIR argument metadata.

Example:

```c
device float* input;
constant float* coefficients;
threadgroup float tile[16][16];
```

---

## What the current compiler enforces

- Device, constant, thread, and threadgroup pointer types lower to their proper AIR address spaces.
- Pointers passed to user functions must belong to the caller's own buffer parameters and match type, address space, struct identity, and matrix width.
- Threadgroup arrays become address-space-3 module globals.
- Atomic buffers use a dedicated atomic wrapper and cannot be treated as ordinary scalar memory.
- `const` locals reject every write (assignment, compound assignment, swizzle stores).
- Whole structs cannot be used as scalar rvalues.
- Pointer arithmetic and indexing are type-directed.
- Texture and sampler parameters are valid only as kernel arguments and method receivers (`read`/`write`/`sample`); matrix-by-value parameters and returns are rejected.

---

## Uniform and varying values

Uniformity matters because a threadgroup barrier must be reached by every lane. BinC currently infers a conservative local classification:

- `coord<N>` values are varying.
- Device-buffer loads are varying.
- Parameters marked `varying` are varying.
- Parameters marked `uniform` and ordinary scalar parameters are uniform by default.
- Expressions involving a varying value are varying.

The compiler warns about varying branches and loop bounds. It rejects a barrier in a region known to be varying:

```c
kernel void bad(device float* data, coord1D i) {
    if (i % 2 == 0) {
        sync(); // error: divergent barrier
    }
}
```

This is intentionally honest: the current implementation does not yet prove every shared-memory race or every interprocedural uniformity fact.

---

## Memory operations

A device load is an ordinary BinC expression:

```c
kernel void copy(device float* in, device float* out) {
    *out = *in;
}
```

A threadgroup access is indexed explicitly and must be synchronized according to the programmer's algorithm:

```c
kernel void tile(device float* out, coord2D c,
                threadgroup float shared[16][16]) {
    shared[c.local.y][c.local.x] = 1.0f;
    sync();
    out[c.global.x] = shared[c.local.y][c.local.x];
}
```

The barrier legality check is implemented. Full read-after-write dominance and bank-conflict analysis remain future work.

---

## Planned type-system work

These are design targets, not current promises:

- Dominance-based proof that a threadgroup read follows a barrier
- Interprocedural uniformity inference beyond varying argument propagation
- Better diagnostics for coalescing and scattered accesses
- Sampler state objects in BinC source (the harness binds a default nearest/clamp sampler)
- Borrow/lifetime checking for shared-memory references
