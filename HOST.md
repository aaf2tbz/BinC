# BinC host seam

BinC is a GPU language. It does not contain Cocoa, event loops, file I/O, or a CPU `main`. A host program supplies the boundary that loads a metallib, creates resources, dispatches kernels, and presents results.

The current project provides a small generated binding layer and a C/Objective-C runtime.

---

## Generated binding headers

When compiling a metallib:

```bash
./binc ../examples/control.binc -o build/control.metallib
```

`binc` also writes:

```text
build/control.h
```

The header contains one inline wrapper for each compute kernel. Device pointers become `BincBuffer*`; scalar kernel arguments remain C scalar values. Coordinate and grid extent parameters are generated as dispatch-owned built-ins and are not exposed as fake host arguments.

A generated wrapper is conceptually:

```c
static inline int binc_update(BincRuntime* rt,
                              size_t grid,
                              BincBuffer* state,
                              BincBuffer* vertices,
                              float dt,
                              float input) {
    BincDispatchArg args[5];
    int n = 0;
    args[n++] = binc_arg_buffer(0, state);
    args[n++] = binc_arg_buffer(1, vertices);
    args[n++] = binc_arg_bytes(2, &dt, sizeof dt);
    args[n++] = binc_arg_bytes(3, &input, sizeof input);
    return binc_runtime_dispatch(rt, "update", grid, args, n);
}
```

The actual header is generated from the parsed BinC function list, so it follows the same parameter order as AIR metadata.

---

## Runtime API

`binc/binc_runtime.h/.m` provides:

- `binc_runtime_open()` and `binc_runtime_close()`
- shared buffer allocation and upload
- shared buffer contents access
- native Metal object access for custom render hosts
- buffer/scalar dispatch argument construction
- compute dispatch with completion waiting

Build the runtime object:

```bash
cd binc
make runtime
```

The runtime is intentionally thin. It does not replace the host language, introduce a second resource schema, or hide the Metal pipeline from applications that need custom rendering.

---

## Pong host

`examples/pong_host.m` is the reference host seam. It does the platform work that should not be in BinC:

1. Create an `NSWindow` and `CAMetalLayer`.
2. Load the generated `build/pong.metallib`.
3. Create shared state and vertex buffers through `BincRuntime`.
4. Read Arrow/W/D key state.
5. Dispatch the BinC `update` kernel.
6. Encode the BinC vertex/fragment pipeline.
7. Present the drawable and depth buffer.
8. Play the full soundtrack and event sound effects.
9. Display game-over text and invoke the restart button.

The game state, physics, geometry, HUD, particle generation, and shaders remain in `examples/pong.binc`.

---

## Building and running Pong

```bash
cd ~/binc/binc
make pong
./pong_host
```

For background testing:

```bash
nohup ./pong_host >pong_host.log 2>&1 < /dev/null &
```

Controls:

- Up Arrow / `W`: up
- Down Arrow / `D`: down
- Escape: quit

---

## Honest limits

The generated API currently targets compute kernels. Render hosts can use the runtime's native Metal handles, as Pong does. A future release can generate typed render-pipeline wrappers and drawable/resource declarations, but that is not required for the current verified game.
