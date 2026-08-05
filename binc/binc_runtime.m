/* Thin Metal 3/4 host seam used by generated BinC headers. */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "binc_runtime.h"
#include <stdlib.h>
#include <string.h>

struct BincRuntime { id<MTLDevice> device; id<MTLCommandQueue> queue; id<MTLLibrary> library; };
struct BincBuffer { id<MTLBuffer> native; };

static int fail(NSError *e){ if(e) fprintf(stderr,"binc runtime: %s\n",e.localizedDescription.UTF8String); return -1; }
BincRuntime *binc_runtime_open(const char *path){
    BincRuntime *r=calloc(1,sizeof *r); r->device=MTLCreateSystemDefaultDevice();
    if(!r->device){ free(r); return NULL; } r->queue=[r->device newCommandQueue];
    NSError *e=nil; r->library=[r->device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]] error:&e];
    if(!r->library){ fail(e); free(r); return NULL; } return r;
}
void binc_runtime_close(BincRuntime *r){ if(r) free(r); }
BincBuffer *binc_buffer_new(BincRuntime *r,const void *p,size_t n){ BincBuffer *b=calloc(1,sizeof *b); b->native=[r->device newBufferWithBytes:p length:n options:MTLResourceStorageModeShared]; return b; }
BincBuffer *binc_buffer_alloc(BincRuntime *r,size_t n){ BincBuffer *b=calloc(1,sizeof *b); b->native=[r->device newBufferWithLength:n options:MTLResourceStorageModeShared]; return b; }
void *binc_buffer_contents(BincBuffer *b){ return b?b->native.contents:NULL; }
void binc_buffer_release(BincBuffer *b){ if(b) free(b); }
BincDispatchArg binc_arg_buffer(int i,BincBuffer *b){ BincDispatchArg a={i,b,NULL,0}; return a; }
BincDispatchArg binc_arg_bytes(int i,const void *p,size_t n){ BincDispatchArg a={i,NULL,p,n}; return a; }
int binc_runtime_dispatch(BincRuntime *r,const char *name,size_t grid,const BincDispatchArg *args,size_t n){
    if(!r||!r->library||!name)return -1; id<MTLFunction> f=[r->library newFunctionWithName:[NSString stringWithUTF8String:name]]; if(!f)return -1;
    NSError *e=nil; id<MTLComputePipelineState> ps=[r->device newComputePipelineStateWithFunction:f error:&e]; if(!ps)return fail(e);
    id<MTLCommandBuffer> cb=[r->queue commandBuffer]; id<MTLComputeCommandEncoder> enc=[cb computeCommandEncoder]; [enc setComputePipelineState:ps];
    for(size_t i=0;i<n;i++){ const BincDispatchArg *a=&args[i]; if(a->buffer)[enc setBuffer:a->buffer->native offset:0 atIndex:a->index]; else [enc setBytes:a->bytes length:a->size atIndex:a->index]; }
    NSUInteger w=ps.threadExecutionWidth; if(!w)w=1; NSUInteger tg=grid<w?w:grid; [enc dispatchThreads:MTLSizeMake(grid,1,1) threadsPerThreadgroup:MTLSizeMake(tg,1,1)]; [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return cb.status==MTLCommandBufferStatusCompleted?0:-1;
}
