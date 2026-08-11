# UE forced-include ordering and `#pragma once`

## Scope and baseline

This fix is based on the authoritative clean audit at commit
`b2e230f288205c864be4c8217c2157d9c6510fe8`:

- compiler SHA-256: `0ab5f22b0d70813677d93461dc458bb7ebc229facb71f1f6e386f8be7efc9893`
- report SHA-256: `6a19358ac68d0921640e0188d89cbdcfe2fca1b21d8e8034b9d70f49d0a30f63`
- audit result: 257 Metallibs, 889 gaps, 0 crashes/hangs/timeouts
- remaining `global name parse`: two rows

One of those rows was `Engine/Shaders/Public/OverloadMacros.ush`. Its
`DECLARE_VECTOR_FUNCTION_OVERLOAD_3_PARAM` calls were left literal because
BinC pre-spliced the main file before processing the forced `Common.ush`:

1. `splice_file(infile)` saw the main file's `#pragma once` and marked it;
2. forced `Common.ush` imported `Platform.ush`, whose
   `#include "/Engine/Public/OverloadMacros.ush"` was skipped as already once;
3. Platform's overload calls were processed before the macro definitions were
   present in the live definition table.

The second remaining row, `PostProcessAmbientOcclusionCommon.ush`, is a
separate context gap: it uses `SCREEN_PASS_TEXTURE_VIEWPORT` without including
`ScreenPass.ush`. This change does not fabricate that missing include.

## Fix

`binc/src/main.c` now processes the forced include first and only then
pre-splices the main HLSL file. This preserves `#pragma once` semantics while
ensuring a main file that is also imported by the forced include contributes
its definitions before the forced include's later call sites are expanded.

## Verification

Tracked fixtures:

- `binc/tests/hlsl/forced_once_defs.hlsl` — `#pragma once` plus a function-like
  macro;
- `binc/tests/hlsl/forced_once_root.hlsl` — imports the definitions, invokes
  the macro, and defines the compute entry point.

Exact red/green evidence:

- canonical `b2e230f` binary: `rc=1`, `expected global name`, no Metallib;
- isolated current-source candidate binary:
  `2d06a1aea9baceb8b10d593471bff20b8a81e8979cae33b5ac058670997b271c`;
- candidate fixture Metallib SHA-256:
  `d6c1466bc6a8148ad9912e0891de4973b8d6beadcb1b37a8a52e7f4a0d3fee1a`;
- exact `OverloadMacros.ush` row under the audit contract: rc=0 with
  Metallib SHA-256 `aa441094e30fe23d192fdad0abcbe3a9a681c84f94ad6fd057468bd5db72a986`.

`make -C binc test-ue-include-order` asserts a nonempty Metallib, and the
broader `test-ue` target depends on it. A fresh full UE audit is required
before this change is considered complete.
