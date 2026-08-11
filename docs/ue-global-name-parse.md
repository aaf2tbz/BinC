# UE global-name parse: left-token-paste preservation

## Scope and baseline

This fix targets the UE/SM6 `global name parse` first-error bucket. The
authoritative pre-fix audit was run with:

- source HEAD: `3aa2d897c76134e607c2536b09846eec3a8bc30b`
- compiler SHA-256: `a855fd6e0f460db53b251ab4344a93b7df4061d150a454ab55e3303700483348`
- report SHA-256: `183eb98fabc8c4125fe6b33b6f20fb2549263928fade14f89513a4b1f84272aa`
- provenance: `build/hlsl-diff/ue-audit.provenance.json`
- runner SHA-256: `8f1cf257ddf5fa31b694b13533e1fbe8f5df10d595a89d9b7c4e0a050a451eac`

The report covered all 1,146 UE rows with `--stage-all`: 255 Metallibs,
891 gaps, and zero crashes, hangs, or timeouts. The `global name parse` bucket
contained 32 rows.

All reproductions use the recorded command contract: the versioned
`tools/hlsl-diff/ue-stubs` include root first, the UE root second,
`Engine/Shaders/Private/Common.ush` forced-included, and the complete SM6
`COMPILER_DXC`/Windows/LWC/color-space define set from the provenance sidecar.

## Root-family classification

Exact replay of the 32 baseline rows identified three families:

| family | rows | evidence |
|---|---:|---|
| `SCREEN_PASS_TEXTURE_VIEWPORT` → `SCREEN_PASS_STRUCT_MEMBER` token paste | 29 | `ScreenPass.ush:5-21`; preprocessed declarations lost the `float2`/struct-name prefix and began as `Extent; ExtentInverse;` |
| literal `DECLARE_VECTOR_FUNCTION_OVERLOAD_3_PARAM(min3, float)` | 1 | unexpanded macro remains in `OverloadMacros.ush`; independent follow-up |
| `globallycoherent RWStructuredBuffer<uint>` | 2 | `MotionBlurFilterTileClassify.usf` and `BloomFindKernel.usf`; independent generated/context or qualifier work |

The fix below addresses only the 29-row token-paste family. It does not claim
to resolve the other three rows/families.

## Fix

In `binc/src/main.c`, function-like macro substitution handles a parameter on
the right side of a `##` paste by copying the text preceding that parameter
into the replacement buffer before removing the adjacent `##` markers.

Before the fix, the left-paste branch removed two bytes from the replacement
buffer without first copying the unprocessed prefix. For:

```hlsl
MemberType StructName##_##MemberName;
```

that discarded `MemberType StructName` and produced malformed declarations.
The corrected branch preserves the prefix, removes only the paste markers,
and then inserts the parameter sentinel.

## Persistent regression

`binc/tests/hlsl/pp_token_paste_regress.hlsl` is the minimal regression. The
tracked Makefile target:

```sh
make -C binc test-ue-global-name-parse
```

runs the fixture with `--stage-all` and asserts that the resulting Metallib is
nonempty. `test-ue` depends on this target as well.

## Red/green evidence

### Minimal fixture at the pre-fix boundary

Fixture SHA-256:
`bee40c5b31f2f7be0bf42dd5d6cf80a43d83aa2fb491865eba3d338df203f63e`.

The pre-fix canonical compiler (`a855fd6e…`) returned `rc=1` with:

```text
binc: error (line 1): expected global name
```

and produced no Metallib. An isolated candidate copied from the exact
`3aa2d89` source, with only this hunk added, built successfully and returned
`rc=0` through AIR and Metallib. Its focused artifact was:

```text
/tmp/current-macro-green.metallib
SHA-256 d6c1466bc6a8148ad9912e0891de4973b8d6beadcb1b37a8a52e7f4a0d3fee1a
```

### Real UE representative

For `Engine/Shaders/Private/FXAAShader.usf` (`ps_5_0`) under the exact audit
contract:

- pre-fix canonical: `rc=1`, `expected global name` at preprocessed line
  10783, no Metallib;
- isolated candidate: `rc=0`, AIR and Metallib emitted;
- landed canonical: `rc=0`, Metallib emitted at
  `/tmp/current-global-name-fxaa-landed.metallib`;
- landed artifact SHA-256:
  `aa441094e30fe23d192fdad0abcbe3a9a681c84f94ad6fd057468bd5db72a986`.

The isolated candidate and landed canonical also passed the local
preprocessor/resource regression set, including `pp_regress.hlsl`,
`macro_token_paste.hlsl`, `static_struct_method.hlsl`, texture-shape/gather,
`resource_field_sample.hlsl`, and `resource_pointer_struct_fields.hlsl`.

## Gate status

The focused persistent gate and real-UE representative pass. The complete
`ci`, differential/golden, and `test-ue` gates plus a fresh authoritative
1,146-row audit are required before this fix can be considered landed in the
UE convergence record.
