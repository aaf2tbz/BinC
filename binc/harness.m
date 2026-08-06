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
//   tex <idx> <w> <h>             RGBA32Float texture at <idx> (compute mode), filled with
//                                 texel(x,y) = (x+1, y+1, x+y+1, 1); kernel writes verified
//                                 by host readback with expecttex <idx> <r> <g> <b> <a>
// Render mode (offscreen pipeline instead of compute):
//   vertex <name> / fragment <name>   stage functions
//   render <w> <h> [nrt]              framebuffer size + color-attachment count
//   draw <n>                          triangle list from buf 0
//   expectpix <rt> <x> <y> <r> <g> <b> <a>   per-pixel render-target check
//   expectdepth <x> <y> <v>                  depth-buffer check
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
    NSString *vertexFn=nil, *fragmentFn=nil;
    long gx=-1, gy=1, gz=1, tx=-1, ty=1, tz=1;
    long rw=-1, rh=-1, nrt=1, nverts=-1;
    id<MTLBuffer> bufs[31]; memset(bufs,0,sizeof bufs);
    id<MTLTexture> texs[31]; memset(texs,0,sizeof texs);
    NSMutableDictionary<NSNumber*,NSArray<NSString*>*> *bufSpec=[NSMutableDictionary dictionary];
    NSMutableDictionary<NSNumber*,NSArray<NSString*>*> *texSpec=[NSMutableDictionary dictionary];
    NSMutableArray<NSNumber*> *expectIdx=[NSMutableArray array];
    NSMutableArray<NSArray<NSString*>*> *expectVals=[NSMutableArray array];
    NSMutableArray<NSNumber*> *expectHex=[NSMutableArray array];
    NSMutableArray<NSNumber*> *dumpIdx=[NSMutableArray array];
    NSMutableDictionary<NSNumber*,NSArray<NSString*>*> *expectTexVals=[NSMutableDictionary dictionary];
    NSMutableArray<NSArray<NSString*>*> *pixExpect=[NSMutableArray array];   /* rt x y r g b a */
    NSMutableArray<NSArray<NSString*>*> *depExpect=[NSMutableArray array];   /* x y v */
    NSMutableArray<NSArray<NSString*>*> *vtxSpec=[NSMutableArray array];     /* loc buffer offset format */
    NSMutableArray<NSNumber*> *dumpPix=[NSMutableArray array];               /* render-target dumps */

    NSCharacterSet *ws=[NSCharacterSet whitespaceAndNewlineCharacterSet];
    for(NSString *line in [spec componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]){
        NSMutableArray<NSString*> *toks=[NSMutableArray array];
        for(NSString *t in [line componentsSeparatedByCharactersInSet:ws])
            if(t.length && ![t hasPrefix:@"#"]) [toks addObject:t]; else if([t hasPrefix:@"#"]) break;
        if(!toks.count) continue;
        NSString *d=toks[0];
        if([d isEqualToString:@"kernel"]){ kernel=toks[1]; }
        else if([d isEqualToString:@"vertex"]){ vertexFn=toks[1]; }
        else if([d isEqualToString:@"fragment"]){ fragmentFn=toks[1]; }
        else if([d isEqualToString:@"render"]){ rw=toks[1].integerValue; rh=toks[2].integerValue; if(toks.count>3) nrt=toks[3].integerValue; }
        else if([d isEqualToString:@"draw"]){ nverts=toks[1].integerValue; }
        else if([d isEqualToString:@"vtx"]){
            /* vtx <location> <buffer> <offset> <format>: vertex attribute layout
             * (builds the MTLVertexDescriptor for stage-in vertex functions) */
            [vtxSpec addObject:[toks subarrayWithRange:NSMakeRange(1,toks.count-1)]]; }
        else if([d isEqualToString:@"dumppix"]){ [dumpPix addObject:@(toks[1].intValue)]; }
        else if([d isEqualToString:@"expectpix"]){ [pixExpect addObject:[toks subarrayWithRange:NSMakeRange(1,toks.count-1)]]; }
        else if([d isEqualToString:@"expectdepth"]){ [depExpect addObject:[toks subarrayWithRange:NSMakeRange(1,toks.count-1)]]; }
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
            texSpec[@(idx)]=toks; // applied after device creation
        }
        else if([d isEqualToString:@"expecttex"]){
            /* expecttex <idx> <r> <g> <b> <a>: every texel must equal these 4 values */
            expectTexVals[@(toks[1].intValue)]=[toks subarrayWithRange:NSMakeRange(2,toks.count-2)];
        }
        else if([d isEqualToString:@"dump"]){ /* dump <idx>: print the buffer as floats, for differential comparisons */
            [dumpIdx addObject:@(toks[1].intValue)];
        }
        else if([d isEqualToString:@"expect"]||[d isEqualToString:@"expecth"]){
            [expectIdx addObject:@(toks[1].intValue)];
            [expectVals addObject:[toks subarrayWithRange:NSMakeRange(2,toks.count-2)]];
            [expectHex addObject:@([d isEqualToString:@"expecth"])]; }
        else die([NSString stringWithFormat:@"unknown directive '%@'",d]);
    }
    if(vertexFn && fragmentFn){ if(rw<0||rh<0) die(@"render spec needs a size"); }
    else if(!kernel||gx<0) die(@"spec needs 'kernel' and a grid directive, or 'vertex'/'fragment' for a render");

    id<MTLDevice> dev=MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q=[dev newCommandQueue];
    NSError *err=nil;
    id<MTLLibrary> lib=[dev newLibraryWithURL:[NSURL fileURLWithPath:libPath] error:&err];
    if(!lib) die([err localizedDescription]);
    id<MTLFunction> f=nil;
    if(kernel) f=[lib newFunctionWithName:kernel];
    if(!f && !(vertexFn&&fragmentFn)) die([NSString stringWithFormat:@"no function %@",kernel]);
    id<MTLComputePipelineState> cps=nil;
    if(f){ cps=[dev newComputePipelineStateWithFunction:f error:&err]; if(!cps) die([err localizedDescription]); }

    for(NSNumber *k in texSpec){
        NSArray<NSString*> *toks=texSpec[k];
        int idx=k.intValue;
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
    }
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
    id<MTLTexture> colorTexs[8]; memset(colorTexs,0,sizeof colorTexs);
    id<MTLTexture> depthTex=nil;
    if(vertexFn && fragmentFn){
        /* ---- render mode: vertex + fragment pipeline ---- */
        if(rw<0||rh<0) die(@"render spec needs a size");
        MTLRenderPipelineDescriptor *rpd=[MTLRenderPipelineDescriptor new];
        rpd.vertexFunction=[lib newFunctionWithName:vertexFn];
        rpd.fragmentFunction=[lib newFunctionWithName:fragmentFn];
        if(!rpd.vertexFunction||!rpd.fragmentFunction) die(@"render functions not found in library");
        /* vertex attribute layout: vtx <location> <buffer> <offset> <format> */
        if(vtxSpec.count){
            MTLVertexDescriptor *vd=[MTLVertexDescriptor new];
            fprintf(stderr,"DBG vtx entries: %zu\n",(size_t)vtxSpec.count);
            int maxend[32]; memset(maxend,0,sizeof maxend);
            for(NSArray *vt in vtxSpec){
                if(vt.count<4) die(@"vtx needs: location buffer offset format");
                int loc=[vt[0] intValue], buf=[vt[1] intValue], off=[vt[2] intValue];
                if(loc<0||loc>30||buf<0||buf>30) die(@"vtx location/buffer out of range");
                MTLVertexFormat vf=MTLVertexFormatFloat4;
                NSString *fmt=vt[3];
                if([fmt isEqualToString:@"float"])vf=MTLVertexFormatFloat;
                else if([fmt isEqualToString:@"float2"]||[fmt isEqualToString:@"f2"])vf=MTLVertexFormatFloat2;
                else if([fmt isEqualToString:@"float3"]||[fmt isEqualToString:@"f3"])vf=MTLVertexFormatFloat3;
                else if([fmt isEqualToString:@"float4"]||[fmt isEqualToString:@"f4"])vf=MTLVertexFormatFloat4;
                else if([fmt isEqualToString:@"half2"]||[fmt isEqualToString:@"h2"])vf=MTLVertexFormatHalf2;
                else if([fmt isEqualToString:@"half4"]||[fmt isEqualToString:@"h4"])vf=MTLVertexFormatHalf4;
                else if([fmt isEqualToString:@"uchar4"]||[fmt isEqualToString:@"u8x4"])vf=MTLVertexFormatUChar4;
                else if([fmt isEqualToString:@"ushort2"]||[fmt isEqualToString:@"u16x2"])vf=MTLVertexFormatUShort2;
                else die([NSString stringWithFormat:@"unknown vtx format %@",fmt]);
                vd.attributes[loc].format=vf; vd.attributes[loc].bufferIndex=(NSUInteger)buf; vd.attributes[loc].offset=(NSUInteger)off;
                fprintf(stderr,"DBG vtx loc=%d buf=%d off=%d fmt=%s\n",loc,buf,off,[fmt UTF8String]);
                int sz=[fmt hasPrefix:@"float4"]||[fmt hasPrefix:@"f4"]?16:[fmt hasPrefix:@"float3"]||[fmt hasPrefix:@"f3"]?12:8;
                if(off+sz>maxend[buf]) maxend[buf]=off+sz;
            }
            for(int b=0;b<32;b++) if(maxend[b]) vd.layouts[b].stride=(NSUInteger)((maxend[b]+3)&~3); /* 4-byte aligned (attribute offsets are) */
            rpd.vertexDescriptor=vd;
        }
        for(int i=0;i<nrt;i++) rpd.colorAttachments[i].pixelFormat=MTLPixelFormatRGBA32Float;
        rpd.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
        id<MTLRenderPipelineState> rps=[dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        if(!rps) die([err localizedDescription]);
        for(int i=0;i<nrt;i++){
            MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:(NSUInteger)rw height:(NSUInteger)rh mipmapped:NO];
            td.usage=MTLTextureUsageRenderTarget;
            colorTexs[i]=[dev newTextureWithDescriptor:td];
        }
        MTLTextureDescriptor *dtd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:(NSUInteger)rw height:(NSUInteger)rh mipmapped:NO];
        dtd.usage=MTLTextureUsageRenderTarget;
        depthTex=[dev newTextureWithDescriptor:dtd];
        MTLRenderPassDescriptor *rpd2=[MTLRenderPassDescriptor renderPassDescriptor];
        for(int i=0;i<nrt;i++){
            rpd2.colorAttachments[i].texture=colorTexs[i];
            rpd2.colorAttachments[i].loadAction=MTLLoadActionClear;
            rpd2.colorAttachments[i].storeAction=MTLStoreActionStore;
            rpd2.colorAttachments[i].clearColor=MTLClearColorMake(0,0,0,0);
        }
        rpd2.depthAttachment.texture=depthTex;
        rpd2.depthAttachment.loadAction=MTLLoadActionClear;
        rpd2.depthAttachment.storeAction=MTLStoreActionStore;
        rpd2.depthAttachment.clearDepth=1.0;
        id<MTLRenderCommandEncoder> renc=[cb renderCommandEncoderWithDescriptor:rpd2];
        [renc setRenderPipelineState:rps];
        [renc setViewport:(MTLViewport){0,0,(double)rw,(double)rh,0,1}];
        /* depth writes are disabled without a depth-stencil state */
        MTLDepthStencilDescriptor *dsd=[MTLDepthStencilDescriptor new];
        dsd.depthWriteEnabled=YES;
        dsd.depthCompareFunction=MTLCompareFunctionAlways;
        id<MTLDepthStencilState> dss=[dev newDepthStencilStateWithDescriptor:dsd];
        [renc setDepthStencilState:dss];
        for(int i=0;i<31;i++) if(bufs[i]) [renc setVertexBuffer:bufs[i] offset:0 atIndex:i];
        for(int i=0;i<31;i++) if(bufs[i]) [renc setFragmentBuffer:bufs[i] offset:0 atIndex:i];
        /* bind a default nearest/clamp sampler at every fragment sampler index
         * (sampler indices are a separate binding space from textures/buffers) */
        MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];
        sd.minFilter=MTLSamplerMinMagFilterNearest; sd.magFilter=MTLSamplerMinMagFilterNearest;
        sd.sAddressMode=MTLSamplerAddressModeClampToEdge; sd.tAddressMode=MTLSamplerAddressModeClampToEdge;
        id<MTLSamplerState> defsamp=[dev newSamplerStateWithDescriptor:sd];
        for(int i=0;i<31;i++) [renc setFragmentSamplerState:defsamp atIndex:i];
        for(int i=0;i<31;i++) if(texs[i]) [renc setFragmentTexture:texs[i] atIndex:i];
        if(nverts<0) die(@"render spec needs a 'draw' directive");
        [renc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)nverts];
        [renc endEncoding];
    } else {
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
    [enc endEncoding];
    }
    [cb commit]; [cb waitUntilCompleted];
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
    /* render-mode pixel and depth verification */
    if(vertexFn && fragmentFn){
        for(NSArray *pe in pixExpect){
            int rt=[pe[0] intValue], x=[pe[1] intValue], y=[pe[2] intValue];
            if(rt<0||rt>=nrt||!colorTexs[rt]) die(@"expectpix target out of range");
            id<MTLTexture> t=colorTexs[rt];
            float *px=malloc((size_t)rw*rh*16);
            [t getBytes:px bytesPerRow:(NSUInteger)rw*16 fromRegion:MTLRegionMake2D(0,0,(NSUInteger)rw,(NSUInteger)rh) mipmapLevel:0];
            for(int c=0;c<4;c++){
                Word expw=parse_word(pe[3+c]); float expf; memcpy(&expf,&expw.bits,4);
                float gotf=px[(y*rw+x)*4+c];
                float tol=1e-4f*(fabsf(expf)+1.0f);
                if(fabsf(gotf-expf)>tol){ printf("  rt%d[%d,%d].%c=%g exp %s X\n",rt,x,y,"rgba"[c],(double)gotf,[pe[3+c] UTF8String]); ok=0; }
            }
            free(px);
        }
        for(NSArray *de in depExpect){
            int x=[de[0] intValue], y=[de[1] intValue];
            float *db=malloc((size_t)rw*rh*4);
            [depthTex getBytes:db bytesPerRow:(NSUInteger)rw*4 fromRegion:MTLRegionMake2D(0,0,(NSUInteger)rw,(NSUInteger)rh) mipmapLevel:0];
            float gotf=db[y*rw+x];
            Word expw=parse_word(de[2]); float expf; memcpy(&expf,&expw.bits,4);
            float tol=1e-4f*(fabsf(expf)+1.0f);
            if(fabsf(gotf-expf)>tol){ printf("  depth[%d,%d]=%g exp %s X\n",x,y,(double)gotf,[de[2] UTF8String]); ok=0; }
            free(db);
        }
    }
    /* differential dump: print whole buffers as floats ('.7g') so two
     * compilations of the same shader can be compared word-for-word */
    for(NSNumber *dn in dumpIdx){
        int idx=dn.intValue;
        if(!bufs[idx]) die([NSString stringWithFormat:@"dump on unbound buffer %d",idx]);
        const uint32_t *w=(const uint32_t*)[bufs[idx] contents];
        NSInteger words=(NSInteger)[bufs[idx] length]/4;
        printf("buf%d:",idx);
        for(NSInteger i=0;i<words;i++){ float f; memcpy(&f,&w[i],4); printf(" %.7g",(double)f); }
        printf("\n");
    }
    /* differential dump of a rendered render-target: pixels as floats */
    for(NSNumber *pn in dumpPix){
        int rt=pn.intValue;
        if(rt<0||rt>=nrt||!colorTexs[rt]) die(@"dumppix target out of range");
        id<MTLTexture> t=colorTexs[rt];
        float *px=malloc((size_t)rw*rh*16);
        [t getBytes:px bytesPerRow:(NSUInteger)rw*16 fromRegion:MTLRegionMake2D(0,0,(NSUInteger)rw,(NSUInteger)rh) mipmapLevel:0];
        printf("pix%d:",rt);
        for(int i=0;i<rw*rh*4;i++) printf(" %.7g",(double)px[i]);
        printf("\n");
        free(px);
    }
    printf(ok?"\n✅ %s correct on GPU\n":"\n❌ %s mismatch\n",[kernel UTF8String]);
    return ok?0:1;
}
