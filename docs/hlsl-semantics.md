# HLSL semantic mapping (BinC HLSL frontend)

How HLSL semantics map onto the BinC/Metal render pipeline. The mapping lives
in `binc/src/hlsl_lower.c` (sem_to_attr); this table is the specification.
Semantics are case-insensitive on both sides.

## Stage-in (vertex inputs, Metal [[attribute(N)]])

| HLSL semantic        | BinC attr   | Metal location |
|----------------------|-------------|----------------|
| POSITION / POSITION0 | user        | 0              |
| TEXCOORDn            | user        | n              |
| COLORn               | user        | 8 + n          |
| NORMAL               | user        | 30             |
| SV_VertexID          | thread      | [[vertex_id]]  |
| SV_InstanceID        | thread      | [[instance_id]]|

COLORn is offset by 8 so TEXCOORD and COLOR registers do not collide in
Metal's single attribute space (D3D9 keeps them in separate register files).

## Stage-out (vertex outputs / fragment inputs)

| HLSL semantic        | BinC attr   | Metal                            |
|----------------------|-------------|----------------------------------|
| SV_Position          | position    | [[position]]                     |
| TEXCOORDn / COLORn   | user        | [[user(locnN)]] (interpolated)   |
| SV_RenderTargetArrayIndex | user  | [[render_target_array_index]]    |
| no_semantic fields   | user        | sequential locn (0, 1, 2, ...)   |

## Fragment outputs

| HLSL semantic   | BinC attr   | Metal          |
|-----------------|-------------|----------------|
| SV_Target0      | color(0)    | [[color(0)]]   |
| SV_TargetN      | color(N)    | [[color(N)]]   |
| SV_Depth        | depth       | [[depth(any)]] |

The lowering synthesizes a `<fn>$out` struct for scalar/vector returns carrying
SV_Target semantics and rewrites the function's `return e;` into a field
assignment + struct return.

## Compute (SV_* thread IDs)

| HLSL semantic       | lowering                                   |
|---------------------|--------------------------------------------|
| SV_DispatchThreadID | coord3D param; reads rewritten to .global  |
| SV_GroupThreadID    | coord3D param; reads rewritten to .local   |
| SV_GroupID          | Phase 5 (derived: .global - .local)        |
| SV_GroupIndex       | Phase 5                                    |

## Interpolation modifiers

| HLSL modifier     | BinC equivalent        | Status |
|-------------------|------------------------|--------|
| linear (default)  | air.perspective        | Phase 3 |
| noperspective     | air.no_perspective     | Phase 3 |
| flat              | air.flat               | Phase 3 |
| centroid / sample | air.center             | Phase 7 |

## Registers

`register(bN)` / `register(tN)` / `register(uN)` are parsed and ignored for
binding; the harness binds buffers/textures by argument position. Register
parity (D3D slot -> Metal index) lands with the Phase-4 reflection sidecar.
