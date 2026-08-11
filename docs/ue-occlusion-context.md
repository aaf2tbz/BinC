# UE generated occlusion context

## Root cause

`OcclusionFeedbackShaders.usf` uses `DummyBuffer` in the non-mobile path:

```hlsl
DummyBuffer[PrimitiveIdx] = 1u;
```

The real ShaderCompilerWorker generates the declaration for this UAV as part
of the per-shader generated uniform-buffer/context input. The standalone UE
corpus has no generated declaration for that shader, so the first include-root
stub must supply the compile-only declaration.

## Candidate change

`tools/hlsl-diff/ue-stubs/GeneratedUniformBuffers.ush` declares:

```hlsl
RWStructuredBuffer<uint> DummyBuffer;
```

behind `BINC_UE_STUB_DUMMY_BUFFER`, preserving idempotent forced-include and
`#pragma once` behavior. The versioned stub remains first in the include order;
the UE checkout remains second.

## Frozen evidence

At boundary `64ff646a76a9e891d0e343929099ae047695fff3`, the row was classified
as `array/index typing` with `subscript of non-pointer`, but the preprocessed
source showed an undeclared generated-context symbol rather than a compiler
array declaration.

A temporary declaration alone changed the exact row to a successful Metallib.
The isolated candidate then passed the focused exact-context target:

```sh
make -C binc test-ue-occlusion-context
```

The target preserves the audit profile, forced `Common.ush`, include order,
and complete UE define set. It emits a nonempty
`/tmp/binc-ue-occlusion-context.metallib`.
