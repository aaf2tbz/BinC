// harness.m — generic GPU dispatch/verify for BinC-compiled metallibs.
//
// usage: harness <lib.metallib> <specfile>
//
// Spec format (whitespace-separated, '#' comments, one directive per line):
//   kernel <name>                 entry point in the metallib
//   grid <N>                      dispatch N threads (single threadgroup)
//   grid2 <W> <H>                 dispatch a 2D grid
//   grid3 <W> <H> <D>             dispatch a 3D grid
//   group <N> / group2/group3     threads per threadgroup (defaults to grid)
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
    long gx=-1, gy=1, gz=1, tx=-1, ty=1, tz=1;
    id<MTLBuffer> bufs[31]; memset(bufs,0,sizeof bufs);
    id<MTLTexture> texs[31]; memset(texs,0,sizeof texs);
    NSMutableDictionary<NSNumber*,NSArray<NSString*>*> *bufSpec=[NSMutableDictionary dictionary];
    NSMutableArray<NSNumber*> *expectIdx=[NSMutableArray array];
    NSMutableArray<NSArray<NSString*>*> *expectVals=[NSMutableArray array];
    NSMutableArray<NSNumber*> *expectHex=[NSMutableArray array];
    NSMutableDictionary<NSNumber*,NSArray<NSString*>*> *expectTexVals=[NSMutableDictionary dictionary];

    NSCharacterSet *ws=[NSCharacterSet whitespaceAndNewlineCharacterSet];
    for(NSString *line in [spec componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]){
        NSMutableArray<NSString*> *toks=[NSMutableArray array];
        for(NSString *t in [line componentsSeparatedByCharactersInSet:ws])
            if(t.length && ![t hasPrefix:@"#"]) [toks addObject:t]; else if([t hasPrefix:@"#"]) break;
        if(!toks.count) continue;
        NSString *d=toks[0];
        if([d isEqualToString:@"kernel"]){ kernel=toks[1]; }
        else if([d isEqualToString:@"grid"]){ gx=toks[1].integerValue; tx=gx; }
        else if([d isEqualToString:@"grid2"]){ gx=toks[1].integerValue; gy=toks[2].integerValue; tx=gx; ty=gy; }
        else if([d isEqualToString:@"grid3"]){ gx=toks[1].integerValue; gy=toks[2].integerValue; gz=toks[3].integerValue; tx=gx; ty=gy; tz=gz; }
        else if([d isEqualToString:@"group"]){ tx=toks[1].integerValue; }
        else if([d isEqualToString:@"group2"]){ tx=toks[1].integerValue; ty=toks[2].integerValue; }
        else if([d isEqualToString:@"group3"]){ tx=toks[1].integerValue; ty=toks[2].integerValue; tz=toks[3].integerValue; }
        else if([d isEqualToString:@"buf"]||[d isEqualToString:@"bufh"]||[d isEqualToString:@"out"]){
            int idx=toks[1].intValue;
            if(idx<0||idx>30) die(@"buffer index out of range");
            bufSpec[@(idx)]=toks; // contents applied after device creation
        }
        else if([d isEqualToString:@"tex"]){
            /* tex <idx> <w> <h>: RGBA32Float texture bound at arg index, filled with
             * texel(x,y) = (x+1, y+1, x+y+1, 1) — deterministic, integer-exact floats */
            int idx=toks[1].intValue;
            if(idx<0||idx>30) die(@"texture index out of range");
            bufSpec[@(idx)]=toks; // applied after device creation
        }
        else if([d isEqualToString:@"expecttex"]){
            /* expecttex <idx> <r> <g> <b> <a>: every texel must equal these 4 values */
            expectTexVals[@(toks[1].intValue)]=[toks subarrayWithRange:NSMakeRange(2,toks.count-2)];
        }
        else if([d isEqualToString:@"expect"]||[d isEqualToString:@"expecth"]){
            [expectIdx addObject:@(toks[1].intValue)];
            [expectVals addObject:[toks subarrayWithRange:NSMakeRange(2,toks.count-2)]];
            [expectHex addObject:@([d isEqualToString:@"expecth"])]; }
        else die([NSString stringWithFormat:@"unknown directive '%@'",d]);
    }
    if(!kernel||gx<0) die(@"spec needs 'kernel' and a grid directive");

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
        if([toks[0] isEqualToString:@"tex"]){
            int w=(int)toks[2].integerValue, h=(int)toks[3].integerValue;
            MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:w height:h mipmapped:NO];
            td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
            id<MTLTexture> t=[dev newTextureWithDescriptor:td];
            float *px=malloc((size_t)w*h*16);
            for(int y=0;y<h;y++) for(int x=0;x<w;x++){
                px[(y*w+x)*4+0]=(float)(x+1); px[(y*w+x)*4+1]=(float)(y+1);
                px[(y*w+x)*4+2]=(float)(x+y+1); px[(y*w+x)*4+3]=1.0f; }
            [t replaceRegion:MTLRegionMake2D(0,0,(NSUInteger)w,(NSUInteger)h) mipmapLevel:0 withBytes:px bytesPerRow:(NSUInteger)w*16];
            free(px);
            texs[idx]=t;
            continue;
        }
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
    for(int i=0;i<31;i++) if(texs[i]) [enc setTexture:texs[i] atIndex:i];
    /* bind a default nearest/clamp sampler at every index that isn't a buffer or texture */
    MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];
    sd.minFilter=MTLSamplerMinMagFilterNearest; sd.magFilter=MTLSamplerMinMagFilterNearest;
    sd.sAddressMode=MTLSamplerAddressModeClampToEdge; sd.tAddressMode=MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> defsamp=[dev newSamplerStateWithDescriptor:sd];
    for(int i=0;i<31;i++) if(!bufs[i]&&!texs[i]) [enc setSamplerState:defsamp atIndex:i];
    [enc dispatchThreads:MTLSizeMake((NSUInteger)gx,(NSUInteger)gy,(NSUInteger)gz)
       threadsPerThreadgroup:MTLSizeMake((NSUInteger)tx,(NSUInteger)ty,(NSUInteger)tz)];
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
    /* texture readback verification: every texel must equal the 4 expected values */
    for(NSNumber *k in expectTexVals){
        int idx=k.intValue;
        if(!texs[idx]) die([NSString stringWithFormat:@"expecttex on unbound texture %d",idx]);
        id<MTLTexture> t=texs[idx];
        NSUInteger w=t.width, h=t.height;
        float *px=malloc(w*h*16);
        [t getBytes:px bytesPerRow:w*16 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
        NSArray<NSString*> *vals=expectTexVals[k];
        Word expw[4]; for(int c=0;c<4&&c<(int)vals.count;c++) expw[c]=parse_word(vals[c]);
        for(NSUInteger y=0;y<h;y++) for(NSUInteger x=0;x<w;x++){
            for(int c=0;c<4&&c<(int)vals.count;c++){
                float gotf=px[(y*w+x)*4+c], expf; memcpy(&expf,&expw[c].bits,4);
                float tol=1e-4f*(fabsf(expf)+1.0f);
                if(fabsf(gotf-expf)>tol){
                    printf("  tex%d[%lu,%lu].%c=%g exp %s X\n",idx,(unsigned long)x,(unsigned long)y,"xyzw"[c],(double)gotf,[vals[c] UTF8String]);
                    ok=0;
                }
            }
        }
        free(px);
        printf("  tex%d readback %lux%lu verified\n",idx,(unsigned long)w,(unsigned long)h);
    }
    printf(ok?"\n✅ %s correct on GPU\n":"\n❌ %s mismatch\n",[kernel UTF8String]);
    return ok?0:1;
}
