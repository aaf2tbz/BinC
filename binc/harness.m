// harness.m — generic GPU dispatch/verify for BinC-compiled metallibs.
//
// usage: harness <lib.metallib> <specfile>
//
// Spec format (whitespace-separated, '#' comments, one directive per line):
//   kernel <name>                 entry point in the metallib
//   grid <N>                      dispatch N threads (single threadgroup)
//   buf <idx> <v0> <v1> ...       buffer bound at <idx>, initialized with words
//   bufh <idx> <bb bb ...>        buffer bound at <idx>, initialized with raw bytes
//                                 (space-separated 2-hex-digit bytes) — for mixed-layout structs
//   out <idx> <nwords>            zero-initialized output buffer at <idx>
//   expect <idx> <v0> <v1> ...    post-run comparison of buffer <idx>
//   expecth <idx> <bb bb ...>     byte-exact post-run comparison of buffer <idx>
//
// A word is a 32-bit value: tokens containing '.', 'e', or 'E' are stored as
// float bits, anything else as int32 bits. `expect` compares int tokens
// exactly and float tokens with a tolerance — so both int and float buffers
// (and structs of either) verify correctly.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static void die(NSString *m){ fprintf(stderr,"harness: %s\n",[m UTF8String]); exit(1); }

typedef struct { uint32_t bits; int is_float; } Word;

static Word parse_word(NSString *tok){
    Word w;
    const char *s=[tok UTF8String];
    w.is_float = strchr(s,'.')||strchr(s,'e')||strchr(s,'E');
    if(w.is_float){ float f=(float)strtod(s,NULL); memcpy(&w.bits,&f,4); }
    else { int32_t v=(int32_t)strtol(s,NULL,0); memcpy(&w.bits,&v,4); }
    return w;
}

int main(int argc, char **argv){
    if(argc<3){ fprintf(stderr,"usage: harness <lib.metallib> <specfile>\n"); return 2; }
    NSString *libPath=[NSString stringWithUTF8String:argv[1]];
    NSString *spec=[NSString stringWithContentsOfFile:[NSString stringWithUTF8String:argv[2]]
                                             encoding:NSUTF8StringEncoding error:nil];
    if(!spec) die(@"cannot read spec file");

    NSString *kernel=nil;
    long grid=-1;
    id<MTLBuffer> bufs[31]; memset(bufs,0,sizeof bufs);
    NSMutableDictionary<NSNumber*,NSArray<NSString*>*> *bufSpec=[NSMutableDictionary dictionary];
    NSMutableArray<NSNumber*> *expectIdx=[NSMutableArray array];
    NSMutableArray<NSArray<NSString*>*> *expectVals=[NSMutableArray array];
    NSMutableArray<NSNumber*> *expectHex=[NSMutableArray array];

    NSCharacterSet *ws=[NSCharacterSet whitespaceAndNewlineCharacterSet];
    for(NSString *line in [spec componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]){
        NSMutableArray<NSString*> *toks=[NSMutableArray array];
        for(NSString *t in [line componentsSeparatedByCharactersInSet:ws])
            if(t.length && ![t hasPrefix:@"#"]) [toks addObject:t]; else if([t hasPrefix:@"#"]) break;
        if(!toks.count) continue;
        NSString *d=toks[0];
        if([d isEqualToString:@"kernel"]){ kernel=toks[1]; }
        else if([d isEqualToString:@"grid"]){ grid=toks[1].integerValue; }
        else if([d isEqualToString:@"buf"]||[d isEqualToString:@"bufh"]||[d isEqualToString:@"out"]){
            int idx=toks[1].intValue;
            if(idx<0||idx>30) die(@"buffer index out of range");
            bufSpec[@(idx)]=toks; // contents applied after device creation
        }
        else if([d isEqualToString:@"expect"]||[d isEqualToString:@"expecth"]){
            [expectIdx addObject:@(toks[1].intValue)];
            [expectVals addObject:[toks subarrayWithRange:NSMakeRange(2,toks.count-2)]];
            [expectHex addObject:@([d isEqualToString:@"expecth"])]; }
        else die([NSString stringWithFormat:@"unknown directive '%@'",d]);
    }
    if(!kernel||grid<0) die(@"spec needs 'kernel' and 'grid'");

    id<MTLDevice> dev=MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q=[dev newCommandQueue];
    NSError *err=nil;
    id<MTLLibrary> lib=[dev newLibraryWithURL:[NSURL fileURLWithPath:libPath] error:&err];
    if(!lib) die([err localizedDescription]);
    id<MTLFunction> f=[lib newFunctionWithName:kernel];
    if(!f) die([NSString stringWithFormat:@"no function %@",kernel]);
    id<MTLComputePipelineState> cps=[dev newComputePipelineStateWithFunction:f error:&err];
    if(!cps) die([err localizedDescription]);

    for(NSNumber *k in bufSpec){
        NSArray<NSString*> *toks=bufSpec[k]; int idx=k.intValue;
        if([toks[0] isEqualToString:@"buf"]){
            NSUInteger nw=toks.count-2; uint32_t *w=malloc(nw*4);
            for(NSUInteger i=0;i<nw;i++) w[i]=parse_word(toks[i+2]).bits;
            bufs[idx]=[dev newBufferWithBytes:w length:nw*4 options:MTLResourceStorageModeShared];
            free(w);
        } else if([toks[0] isEqualToString:@"bufh"]){
            NSUInteger nb=toks.count-2; uint8_t *b=malloc(nb?nb:1);
            for(NSUInteger i=0;i<nb;i++) b[i]=(uint8_t)strtoul([toks[i+2] UTF8String],NULL,16);
            bufs[idx]=[dev newBufferWithBytes:b length:nb options:MTLResourceStorageModeShared];
            free(b);
        } else {
            NSUInteger nw=toks[2].integerValue;
            bufs[idx]=[dev newBufferWithLength:nw*4 options:MTLResourceStorageModeShared];
        }
        if(!bufs[idx]) die(@"buffer allocation failed");
    }

    id<MTLCommandBuffer> cb=[q commandBuffer];
    id<MTLComputeCommandEncoder> enc=[cb computeCommandEncoder];
    [enc setComputePipelineState:cps];
    for(int i=0;i<31;i++) if(bufs[i]) [enc setBuffer:bufs[i] offset:0 atIndex:i];
    [enc dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(grid,1,1)];
    [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if(cb.status!=MTLCommandBufferStatusCompleted) die(@"GPU command buffer failed");

    int ok=1;
    for(NSUInteger e=0;e<expectIdx.count;e++){
        int idx=expectIdx[e].intValue; NSArray<NSString*> *vals=expectVals[e];
        if(!bufs[idx]) die([NSString stringWithFormat:@"expect on unbound buffer %d",idx]);
        if(expectHex[e].boolValue){
            const uint8_t *b=(const uint8_t*)[bufs[idx] contents];
            for(NSUInteger i=0;i<vals.count;i++){
                unsigned expb=(unsigned)strtoul([vals[i] UTF8String],NULL,16);
                int pass=b[i]==expb;
                printf("  buf%d[%lu]=0x%02x exp 0x%02x %s\n",idx,(unsigned long)i,b[i],expb,pass?"OK":"X");
                if(!pass)ok=0;
            }
            continue;
        }
        const uint32_t *w=(const uint32_t*)[bufs[idx] contents];
        for(NSUInteger i=0;i<vals.count;i++){
            Word exp=parse_word(vals[i]);
            float gotf, expf; int32_t goti, expi;
            memcpy(&gotf,&w[i],4); memcpy(&expf,&exp.bits,4);
            memcpy(&goti,&w[i],4); memcpy(&expi,&exp.bits,4);
            int pass;
            if(exp.is_float){
                float tol=1e-4f*(fabsf(expf)+1.0f); pass=fabsf(gotf-expf)<=tol;
                printf("  buf%d[%lu]=%g exp %s %s\n",idx,(unsigned long)i,(double)gotf,[vals[i] UTF8String],pass?"OK":"X");
            } else {
                pass=(goti==expi);
                printf("  buf%d[%lu]=%d exp %s %s\n",idx,(unsigned long)i,goti,[vals[i] UTF8String],pass?"OK":"X");
            }
            if(!pass)ok=0;
        }
    }
    printf(ok?"\n✅ %s correct on GPU\n":"\n❌ %s mismatch\n",[kernel UTF8String]);
    return ok?0:1;
}
