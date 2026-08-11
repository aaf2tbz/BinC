# UE unsized local array initializers

## Current-boundary evidence

At the authoritative `78f84713084a9608603614ad39dba5ada7cbd7c6` boundary
(compiler SHA-256 `d2e4753d71c5de8ac706603e2f58da00572df9790a3d7076be3d117f24a58e5f`,
audit report SHA-256
`911997a163b96e226ab68dc0c6aaa62cc54b8c749ca2f2ef49dbf77c43357621`), the
`array/index typing` bucket contained five rows. The canonical compiler
reproduced:

```text
binc: error (...): subscript of non-pointer
```

for `PrintValue.ush`, whose functions use HLSL declarations of the form:

```hlsl
const int digits[] = { 480599, 139810, ... };
CharBin = digits[Digit];
```

The parser represented the unsized extent as `array_n=0`. Local-array codegen
therefore registered a scalar local, and indexed access reached the generic
non-pointer diagnostic.

## Fix

Shared local parsing now preserves brace-list expressions only for explicit or
unsized array declarations. Unsized arrays infer `array_n` from the top-level
initializer count. Other brace initializers, such as `float3 v = {...}`, retain
the previous discard behavior and are not misclassified as arrays.

Codegen allocates the inferred `[N x T]` local and emits typed element GEPs and
stores before the local is used. Initialization stores bypass `gen_lval`, so a
`const` array can be initialized without making later writes legal.
Nested local array initializers remain an explicit unsupported diagnostic.

## Proof

The tracked regression is:

```text
binc/tests/hlsl/unsized_local_array_regress.hlsl
make -C binc test-ue-array-index
```

The isolated candidate produced a valid Metallib for the fixture (SHA-256
`d6c1466bc6a8148ad9912e0891de4973b8d6beadcb1b37a8a52e7f4a0d3fee1a`). Under
the complete UE audit contract, `PrintValue.ush` advanced from the red
`subscript of non-pointer` diagnostic to a valid Metallib (candidate output
SHA-256 `aa441094e30fe23d192fdad0abcbe3a9a681c84f94ad6fd057468bd5db72a986`).

The other four current array/index rows retain their original first error in
the isolated candidate and remain open; this change is intentionally limited
to unsized local array initialization.
