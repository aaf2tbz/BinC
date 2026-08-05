# BinC verified facts

These notes record observations made against the installed Apple toolchain. They are evidence for the emitter, not a promise that every probed feature is implemented in the BinC parser.

## Toolchain

- Xcode 27 beta 4 was used during the original probes.
- `metal -emit-llvm` produces AIR LLVM IR for the `air64` target.
- `.air` is LLVM bitcode inside an Apple wrapper.
- `metallib` links AIR into a Metal executable.
- Generic host-target clang bitcode is rejected by `metallib`.
- The old `metal-ir-converter` path is not available in this toolchain.
- Metal 4 adds a modern host API surface; it does not replace MSL with a new shader language.

## AIR address spaces

| Address space | Meaning |
|---:|---|
| 0 | thread/private |
| 1 | device |
| 2 | constant/sampler |
| 3 | threadgroup |
| 7 | mesh payload in the probed Metal 4 mesh contract |

## Probed compute contracts

- `air.thread_position_in_grid` provides a 1-D or vector coordinate.
- `air.thread_position_in_threadgroup` provides a local coordinate.
- `air.threadgroup_position_in_grid` provides a group coordinate.
- `air.threads_per_grid` provides the grid extent.
- Threadgroup storage is represented by module-level address-space-3 globals.
- Atomic buffers use a `metal::_atomic` wrapper and `air.atomic.global.add.*` intrinsics.
- Texture resources use opaque texture structs and `air.read_texture_2d.*`/`air.write_texture_2d.*` intrinsics.

## Probed render contracts

- Compute, vertex, fragment, and mesh stages are registered through named `!air.*` metadata nodes.
- Vertex position outputs use `air.position` and vertex output metadata.
- Fragment position inputs use `air.position` plus interpolation metadata.
- Fragment outputs use `air.render_target` metadata.
- Mesh stages use address space 7 and mesh-specific metadata/intrinsics.

## Current BinC status

Implemented and GPU-verified in the repository:

- compute kernels and explicit coordinate domains
- threadgroup arrays and barriers
- atomic add
- generated host bindings/runtime
- position-only vertex/fragment stages
- the playable Pong pipeline

Still only probed/documented, not general BinC syntax:

- textures and samplers
- arbitrary stage-output structs/interpolants
- mesh/object shaders
- ray tracing
- Metal 4 host command encoders beyond the small runtime seam

When changing AIR emission, compare the generated `.ll` against the relevant reference file and run `cd binc && make verify`.
