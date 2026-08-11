# UE `log10` intrinsic

HLSL exposes `log10`, while the supported AIR math surface provides
`air.fast_log.f32`. BinC lowers `log10(x)` as `log(x) * 0.4342944819032518`
(`1 / ln(10)`) for scalar and vector operands, applying the AIR logarithm
lane by lane for vectors.

The regression `binc/tests/hlsl/log10_intrinsic_regress.hlsl` covers both
scalar and `float3` calls and requires a nonempty Metallib.
