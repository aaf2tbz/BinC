# UE static/const local qualifiers

## Scope

The UE parser bucket `parse: expected ;` includes block-scope declarations such
as:

```hlsl
static const float Scale = 2.0;
static const int Digits[3] = { 1, 2, 3 };
```

The implementation consumes `static` and `const` together before scalar,
vector, array, and struct local declarations. Static locals remain
per-invocation AIR locals; `const` is retained in statement metadata so writes
remain rejected.

## Frozen evidence

At boundary `64ff646a76a9e891d0e343929099ae047695fff3`, the exact bucket had
29 rows. The canonical minimal static declaration failed with:

```text
binc: error (line 2): expected ;
```

The current-HEAD candidate produces a nonempty Metallib for scalar and fixed
array static-const locals. Its const-write probe remains rejected with
`cannot write to const local value`. The frozen 29-row replay produces one
Metallib and moves the remaining rows to later independent parser/type,
context, resource, and intrinsic failures; this wave does not claim all 29
compile yet.

## Focused proof

```sh
make -C binc test-ue-static-local-qualifiers
```

The regression covers a scalar and a fixed array, and the separate semantic
probe verifies that `static const` remains write-protected.
