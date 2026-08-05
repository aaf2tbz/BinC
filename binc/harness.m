// harness.m — dispatch BinC-compiled metallibs on the GPU and verify correctness.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static void die(NSString *m){ fprintf(stderr,"harness: %s\n",[m UTF8String]); exit(1); }

typedef struct { float x,y,z, vx,vy,vz; } Particle;

int main(int argc, char **argv){
    if(argc<3){ fprintf(stderr,"usage: harness <lib.metallib> <blend|step>\n"); return 2; }
    NSString *libPath=[NSString stringWithUTF8String:argv[1]];
    NSString *fn=[NSString stringWithUTF8String:argv[2]];

    id<MTLDevice> dev=MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q=[dev newCommandQueue];
    NSError *err=nil;
    id<MTLLibrary> lib=[dev newLibraryWithURL:[NSURL fileURLWithPath:libPath] error:&err];
    if(!lib) die([err localizedDescription]);
    id<MTLFunction> f=[lib newFunctionWithName:fn];
    if(!f) die(([NSString stringWithFormat:@"no function %@",fn]));
    id<MTLComputePipelineState> cps=[dev newComputePipelineStateWithFunction:f error:&err];
    if(!cps) die([err localizedDescription]);

    id<MTLCommandBuffer> cb=[q commandBuffer];
    id<MTLComputeCommandEncoder> enc=[cb computeCommandEncoder];
    [enc setComputePipelineState:cps];

    if([fn isEqualToString:@"blend"]){
        const int N=4; float a[4]={1,2,3,4},b[4]={10,20,30,40};
        id<MTLBuffer> ba=[dev newBufferWithBytes:a length:N*4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bb=[dev newBufferWithBytes:b length:N*4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bo=[dev newBufferWithLength:N*4 options:MTLResourceStorageModeShared];
        [enc setBuffer:ba offset:0 atIndex:0]; [enc setBuffer:bb offset:0 atIndex:1]; [enc setBuffer:bo offset:0 atIndex:2];
        [enc dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(N,1,1)];
        [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
        float *r=(float*)[bo contents]; int ok=1;
        for(int i=0;i<N;i++){ float e=(a[i]+b[i])*0.5f; printf("  out[%d]=%.3f exp %.3f %s\n",i,r[i],e,r[i]==e?"OK":"X"); if(r[i]!=e)ok=0; }
        printf(ok?"\n✅ blend correct on GPU\n":"\n❌ blend mismatch\n"); return ok?0:1;
    } else if([fn isEqualToString:@"step"]){
        const int N=2;
        Particle p[2]={ {0,0,0, 1,2,3}, {10,10,10, 0.5f,0.5f,0.5f} };
        float dt=2.0f;
        id<MTLBuffer> bp=[dev newBufferWithBytes:p length:N*sizeof(Particle) options:MTLResourceStorageModeShared];
        id<MTLBuffer> bdt=[dev newBufferWithBytes:&dt length:4 options:MTLResourceStorageModeShared];
        [enc setBuffer:bp offset:0 atIndex:0]; [enc setBuffer:bdt offset:0 atIndex:1];
        [enc dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(N,1,1)];
        [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
        Particle *r=(Particle*)[bp contents]; int ok=1;
        Particle exp[2]={ {2,4,6, 1,2,3}, {11,11,11, 0.5f,0.5f,0.5f} };
        for(int i=0;i<N;i++){ Particle e=exp[i];
            printf("  p%d pos=(%.1f,%.1f,%.1f) exp (%.1f,%.1f,%.1f) %s\n",i,r[i].x,r[i].y,r[i].z,e.x,e.y,e.z,
                (r[i].x==e.x&&r[i].y==e.y&&r[i].z==e.z)?"OK":"X");
            if(r[i].x!=e.x||r[i].y!=e.y||r[i].z!=e.z)ok=0; }
        printf(ok?"\n✅ step correct on GPU (struct fields + scalar param lowering verified)\n":"\n❌ step mismatch\n");
        return ok?0:1;
    } else if([fn isEqualToString:@"sc"]){
        const int N=4; float a[4]={1,5,10,20};
        float k=2,lo=3,hi=15;
        id<MTLBuffer> ba=[dev newBufferWithBytes:a length:N*4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bo=[dev newBufferWithLength:N*4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bk=[dev newBufferWithBytes:&k length:4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bl=[dev newBufferWithBytes:&lo length:4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bh=[dev newBufferWithBytes:&hi length:4 options:MTLResourceStorageModeShared];
        [enc setBuffer:ba offset:0 atIndex:0]; [enc setBuffer:bo offset:0 atIndex:1];
        [enc setBuffer:bk offset:0 atIndex:2]; [enc setBuffer:bl offset:0 atIndex:3]; [enc setBuffer:bh offset:0 atIndex:4];
        [enc dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(N,1,1)];
        [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
        float *r=(float*)[bo contents]; int ok=1; float exp[4]={3,10,15,15};
        for(int i=0;i<N;i++){ printf("  sc[%d]=%.3f exp %.3f %s\n",i,r[i],exp[i],r[i]==exp[i]?"OK":"X"); if(r[i]!=exp[i])ok=0; }
        printf(ok?"\n✅ sc correct on GPU (locals + for + if/else + integers)\n":"\n❌ sc mismatch\n");
        return ok?0:1;
    }
    fprintf(stderr,"harness: unknown function %s\n",argv[2]); return 2;
}
