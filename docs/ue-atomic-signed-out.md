# UE atomic signed/unsigned out results

## Scope

HLSL permits the out result of an integer `InterlockedAdd` to use the other
32-bit signedness, for example a `uint` UAV with an `int` out variable:

```hlsl
int Previous;
InterlockedAdd(Counter[0], 1, Previous);
```

The AIR atomic ABI uses the same 32-bit integer representation for `int` and
`uint`. The implementation allows only this signed/unsigned integer alias;
vector, matrix, float, and other mismatches remain rejected.

## Frozen evidence

At the `16758a63db8f7907e1c6b025186d4109143ae962` boundary, the fresh UE
ledger contains 18 exact rows whose first error is:

```text
atomic add out result type mismatch
```

The isolated candidate advances all 18 rows to the later generated/helper
failure `undefined function ShaderPrintGetOffset`, without crashes or hangs.
No full-UE Metallib advancement is claimed for those rows by this wave; the
later helper family remains separate.

## Focused proof

`tests/hlsl/atomic_signed_out_regress.hlsl` exercises a `uint` UAV with an
`int` `InterlockedAdd` out result and must produce a nonempty Metallib:

```sh
make -C binc test-ue-atomic-signed-out
```
