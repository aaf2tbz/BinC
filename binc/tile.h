#ifndef BINC_tile_H
#define BINC_tile_H
#include <stdint.h>
#include <stddef.h>
#include "binc_runtime.h"

static inline int binc_tile(BincRuntime *rt, size_t grid, BincBuffer *out){ BincDispatchArg a[4]; int n=0;
    a[n++]=binc_arg_buffer(0, out);
    return binc_runtime_dispatch(rt, "tile", grid, a, n); }

#endif
