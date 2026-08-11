# UE uniform-capacity crash regression

## Root cause

After the left-token-paste fix accepted declarations in large UE shader
modules, `binc/src/hlsl_lower.c:hlsl_build` reached the D3D9 uniform packer
with more than 64 non-const globals/`uniform` parameters. The fixed
`Field uf[64]` array was written past its stack boundary at the `uf[nuf++]`
append. The exact post-token-paste rows were:

- `PostProcessCompositePrimitives.usf`, `MainTemporalUpsampleDepthPS`
- `PostProcessMaterialShaders.usf`, `MainPS`

The pre-fix binary (`a855fd6e…`) stopped earlier with `expected global name`.
The token-paste binary (`393c5f95…`) reached `hlsl_build` and crashed with
signal 11 on both rows.

An isolated ASan build reproduced the memory error:

```text
AddressSanitizer: stack-buffer-overflow
WRITE of size 104
hlsl_lower.c:1019 in hlsl_build
'uf' [10032, 16688) ... memory access at offset 16688
```

## Fix

`uf` is now allocated for `hp->nglobals + 1 + sum(function parameter counts)`
and freed after the copied `$uniforms` struct and rewrite records are built.
The allocation is bounded by the parsed program rather than an arbitrary
fixed stack capacity.

## Verification

The tracked `binc/tests/ue_uniform_capacity_regress.sh` gate runs both exact
UE rows with the audit contract:

- versioned stub include root first, UE root second;
- forced `Common.ush`;
- the complete audit defines;
- `--stage-all -T ps_5_0`.

It accepts ordinary compiler diagnostics but fails on signal return codes or
crash signatures. With the fix, both rows return rc=1 clean diagnostics and
the gate prints:

```text
UE UNIFORM CAPACITY NON-CRASH PASS
```

The broader `test-ue` target depends on this gate. The rows remain UE context
gaps rather than being claimed as Metallib passes; the gate specifically
prevents a crash while their missing generated context and unsupported
features are repaired separately.
