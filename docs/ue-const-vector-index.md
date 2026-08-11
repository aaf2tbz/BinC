# UE constant-vector indexing

## Failure

UE ACES helpers use module constants such as:

```hlsl
static const float3 surround = float3(0.9, 0.59, 0.9);
float J = 100.0 * pow(A / A_w, surround[1] * z);
```

The expression type resolver previously retained only `kind` for `R_CONST`
identifiers. A vector constant therefore looked scalar to the value-index
lowering and fell through to pointer indexing with `subscript of non-pointer`.

## Lowering

Constant identifiers now preserve their complete `Type` metadata, including
vector width, matrix dimensions, array extents, and struct tags. The existing
HLSL value-index path then emits an AIR/LLVM `extractelement`; pointer/lvalue
indexing remains reserved for actual storage.

## Regression

`binc/tests/hlsl/const_vector_index_regress.hlsl` indexes a `static const
float3` with a dynamic scalar lane and must produce a nonempty Metallib. The
focused gate is `make test-ue-const-vector-index`.

## Audit evidence

At the frozen `16758a6` and `311cf85` boundaries, both
`ACESDisplayEncoding.ush` and `ACESOutputTransform.ush` first failed with
`subscript of non-pointer` at the constant vector index. The isolated
candidate advanced both rows beyond that error; later failures remain separate
buckets and must be audited independently.
