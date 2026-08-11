# UE `all(boolN)` intrinsic lowering

## Scope and baseline

The authoritative pre-fix audit boundary is commit
`06d3fdd586e382bdcd712ec29998eebc37907ee0`:

- compiler SHA-256: `2d06a1aea9baceb8b10d593471bff20b8a81e8979cae33b5ac058670997b271c`
- report SHA-256: `56eaa0bf1d9937a0521a183328c5bf38cab8fdfc5c0ccc373cf76a8c09fc4fee`
- audit result: 261 Metallibs, 885 gaps, 0 crashes/hangs/timeouts
- `undefined function (all)`: 18 rows

The exact current canonical fixture failed with:

```text
binc: error (line 9, col 17): undefined function all
```

and emitted no Metallib.

## Fix

`binc/src/codegen.c` now treats `all` as an HLSL composite intrinsic:

- expression typing returns scalar `bool` for `all(boolN)`;
- `all` is included in the composite-math dispatch table;
- vector masks are reduced with LLVM `extractelement` and scalar `and` operations;
- scalar `bool` passes through unchanged;
- non-boolean arguments receive a clean diagnostic.

This is deliberately limited to `all`; residual `any`, resource indexing, and
other later diagnostics remain separate buckets.

## Verification

Tracked fixture:

- `binc/tests/hlsl/all_bool_vector_regress.hlsl`

Current-source isolated candidate:

- candidate binary SHA-256: `cc914aae871d4ba954ca33b741261161802d563294e9e85e992031d200cd4bfa`
- fixture Metallib SHA-256: `04ede4556bc09f2901ef466060058e941ad023563979393b9fbda6359c788499`
- AIR was emitted and accepted by `xcrun metal`; `metallib` linked successfully.

A representative real UE row was replayed under the full audit contract:

- `DistanceFieldDownsampling.usf`, canonical: first error `undefined function all`;
- the current-source candidate: first error advanced to
  `subscript of non-pointer`.

The latter is an independent `array/index typing` bucket, not evidence of
full-row closure by this intrinsic fix.

`make -C binc test-ue-all-intrinsic` requires a nonempty Metallib and is a
dependency of `test-ue`. The complete native gate and a fresh full UE audit
are required before this change is considered complete.
