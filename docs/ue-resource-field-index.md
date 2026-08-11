# UE direct resource-field indexing

## Scope

This wave lowers direct indexing of a resource field held in a struct value or
struct parameter, for example:

```hlsl
struct ResourceBundle { Buffer<float4> Data; }
float Read(ResourceBundle Bundle) { return Bundle.Data[0].x; }
```

The field handle is loaded from the struct field address, then an element GEP is
emitted in the resource’s declared address space. Existing pointer-expression
paths remain unchanged.

Struct assignment through an indexed local array also preserves the RHS
struct tag while lowering the integer index expression; this prevents the
index expression’s scalar type state from erasing the RHS struct identity.

## Frozen evidence

At the `8532747f57c43c32a9901c2416b3de8fe501874f` boundary, the
`unsupported pointer expression` family contained 10 exact UE rows across
IES/rect-light, virtual-shadow, distance-field, hair, Lumen, and ray-tracing
shaders. The candidate advances all 10 rows to:

```text
atomic add out result type mismatch
```

No full-UE Metallib advancement is claimed for those rows at this boundary;
the later atomic result-type family is separate.

## Focused proof

`tests/hlsl/resource_field_direct_index_regress.hlsl` exercises direct
`Buffer<float4>` field indexing and produces a nonempty Metallib. The existing
`resource_pointer_struct_fields.hlsl` regression also remains green.

```sh
make -C binc test-ue-resource-field-index
```
