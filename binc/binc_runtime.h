/* Minimal host seam for compiler-generated BinC bindings. Compile binc_runtime.m
 * with the same Xcode/Metal SDK used to build the harness. */
#ifndef BINC_RUNTIME_H
#define BINC_RUNTIME_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct BincRuntime BincRuntime;
typedef struct BincBuffer BincBuffer;
typedef struct {
    int index;
    BincBuffer *buffer;
    const void *bytes;
    size_t size;
} BincDispatchArg;

BincRuntime *binc_runtime_open(const char *metallib_path);
void binc_runtime_close(BincRuntime *rt);
BincBuffer *binc_buffer_new(BincRuntime *rt, const void *bytes, size_t size);
BincBuffer *binc_buffer_alloc(BincRuntime *rt, size_t size);
void *binc_buffer_contents(BincBuffer *buffer);
void binc_buffer_release(BincBuffer *buffer);
BincDispatchArg binc_arg_buffer(int index, BincBuffer *buffer);
BincDispatchArg binc_arg_bytes(int index, const void *bytes, size_t size);
int binc_runtime_dispatch(BincRuntime *rt, const char *kernel, size_t grid,
                          const BincDispatchArg *args, size_t nargs);
#ifdef __cplusplus
}
#endif
#endif
