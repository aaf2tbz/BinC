# UE `Texture2D.GetDimensions`

HLSL `Texture2D.GetDimensions(mip, width, height, mipCount)` writes three
`uint` out-lvalues. The lowering uses the installed Metal AIR queries:

- `air.get_width_texture_2d(texture, mip)`
- `air.get_height_texture_2d(texture, mip)`
- `air.get_num_mip_levels_texture_2d(texture)`

The focused regression covers a `uint3` swizzle destination (`Dims.x/y/z`)
and requires a nonempty Metallib. This wave is intentionally limited to the
2D non-array, non-cube overload; other texture shapes remain explicit gaps
until separately probed and tested.
