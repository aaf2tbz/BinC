# UE array-parameter lowering

## Scope

This note covers ordinary HLSL fixed-array parameters such as:

```hlsl
float Interpolate(FAVSMSampleData ShadowData[2], float WorldHitT)
{
    return ShadowData[0].Tau + ShadowData[1].X + WorldHitT;
}
```

These parameters are ordinary thread-address-space pointers after HLSL array
argument decay. They are distinct from `groupshared` arrays, which remain
module-level `addrspace(3)` storage and are not ABI arguments.

## Frozen evidence

At the `64ff646a76a9e891d0e343929099ae047695fff3` UE boundary, the four-row
`array/index typing` bucket replayed as:

- `OcclusionFeedbackShaders.usf`: `subscript of non-pointer` on generated
  `DummyBuffer` context; this remains a context/stub case.
- Three HeterogeneousVolumes rows: `subscript of non-pointer` on helper
  parameters such as `FAVSMSampleData ShadowData[2]`.

The isolated candidate compiler changed the three helper rows' first error to
`unsupported pointer expression` at the later resource expression
`ShadowMap.IndirectionBuffer[LinearIndex]`. It did not make those full UE rows
compile, so no Metallib advancement is claimed for them at this boundary.

## Lowering contract

The candidate keeps `AS_THREADGROUP` array parameters as synthetic shared
memory. Ordinary HLSL array parameters are emitted as pointers to their element
type. Local fixed arrays decay through a first-element GEP when passed to such
parameters; already-pointer array parameters are forwarded unchanged. Local
arrays of structs use normal per-element alloca/GEP/store lowering.

## Focused proof

`tests/hlsl/array_parameter_regress.hlsl` exercises:

- struct array parameter indexing;
- local struct-array initialization;
- forwarding an array parameter to another helper;
- a nonempty Metallib result.

The candidate AIR contains a helper ABI of the form:

```text
%struct.SampleData addrspace(0)* %_ShadowData
```

and indexes it with element GEPs. The focused Make target is:

```sh
make -C binc test-ue-array-parameters
```

The canonical implementation passed the focused regression and the complete
native gate before this documentation was committed.
