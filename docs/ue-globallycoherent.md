# UE `globallycoherent` qualifier parsing

## Scope and baseline

The authoritative baseline for this fix is commit `35ce6d0176178913f72bb4af10099532ac440a60`:

- compiler SHA-256: `23ad8674bb4872179813008b28c0def66a7fb84f53a091a8ab3bf31988bb9e08`
- report SHA-256: `f52467da1caf316a2a839af1c098742395c84a97f6bed160cb5f20780fd96832`
- audit result: 257 Metallibs, 889 gaps, 0 crashes/hangs/timeouts

Four rows remained in `global name parse`. The MotionBlur and Bloom rows
use the UE macro:

```hlsl
#define RWCoherentStructuredBuffer(TYPE) globallycoherent RWStructuredBuffer<TYPE>
```

The canonical compiler reported `expected global name` on those declarations
because `globallycoherent` was an unhandled identifier before the resource
type. The other remaining rows—`OverloadMacros.ush` and
`PostProcessAmbientOcclusionCommon.ush`—are independent overload-macro
expansion cases and are not claimed fixed here.

## Fix

`binc/src/hlsl_parser.c:hlsl_type` now consumes `globallycoherent` alongside
other harmless HLSL variable modifiers (`row_major`, `precise`, and `shared`).
The qualifier is intentionally not represented in the Metal type: it affects
D3D memory-ordering semantics, not the parser’s resource shape, and the
current BinC lowering path has no equivalent qualifier to emit.

## Verification

Tracked fixture: `binc/tests/hlsl/globallycoherent_regress.hlsl`.

- pre-fix canonical boundary: `rc=1`, `expected global name`, no Metallib;
- isolated candidate binary (current source plus this hunk):
  `0ab5f22b0d70813677d93461dc458bb7ebc229facb71f1f6e386f8be7efc9893`;
- candidate fixture Metallib SHA-256:
  `3837fa2ec18c4fe7a82cf7d66335222027cb8644213160fc3fa5f3b1917a75b2`;
- exact MotionBlur and Bloom rows advance past `global name parse` to
  ordinary diagnostics with no signal/crash.

`make -C binc test-ue-globallycoherent` asserts a nonempty Metallib, and the
broader `test-ue` target depends on it. A fresh full UE audit is required
before this change is considered complete.
