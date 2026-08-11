# UE `any(boolN)` intrinsic lowering

## Scope

HLSL `any` reduces a scalar or vector boolean expression to one scalar bool.
For a vector, the lowering is an `or` reduction across the lanes; for a
scalar, the value is returned unchanged.

## Frozen evidence

At the `4f55a18054f245c2d7a74d98a9c213ec30da7f06` boundary, the authoritative
UE audit contained exactly two `undefined function (any)` rows:

- `PathTracing/PathTracingBuildAdaptiveError.usf`
- `RayTracing/RayTracingBuildDecalGrid.usf`

The canonical minimal repro was:

```hlsl
float3 value = float3(1.0, -2.0, 3.0);
bool3 mask = value > 0.0;
bool result = any(mask);
```

It failed with `undefined function any`. The isolated candidate emits
`extractelement` plus `or i1` reductions and produces a nonempty Metallib.
The two exact UE rows advance to later independent failures, respectively
`vector width mismatch in comparison` and the texture-indexing gap; this note
does not claim those rows compile fully.

## Focused proof

```sh
make -C binc test-ue-any-intrinsic
```

The focused AIR contains the expected lane reduction:

```text
%t20 = or i1 %t18, %t19
%t22 = or i1 %t20, %t21
```
