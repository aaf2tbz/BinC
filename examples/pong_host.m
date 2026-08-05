/* Host seam for the 3-D BinC Pong game. Simulation, arena geometry, HUD,
 * vertex stage, fragment atmosphere, rounds, and lives are all in pong.binc. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AVFoundation/AVFoundation.h>
#include "../binc/binc_runtime.h"
#include "../binc/build/pong.h"

static BOOL g_up, g_down;
static BOOL g_running=YES;

typedef struct { float ballx,bally,ballz,vx,vy,vz,left,right,trail,clock,round_time; int round,lives,flash,event,player_score,ai_score; } HostGame;

@interface PongView : NSView @end
@implementation PongView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)e {
    NSString *s=e.charactersIgnoringModifiers.lowercaseString;
    if(e.keyCode==126 || [s containsString:@"w"]) g_up=YES;
    if(e.keyCode==125 || [s containsString:@"d"]) g_down=YES;
}
- (void)keyUp:(NSEvent *)e {
    NSString *s=e.charactersIgnoringModifiers.lowercaseString;
    if(e.keyCode==126 || [s containsString:@"w"]) g_up=NO;
    if(e.keyCode==125 || [s containsString:@"d"]) g_down=NO;
}
@end

static id<MTLTexture> makeDepth(id<MTLDevice> dev, NSUInteger w, NSUInteger h){
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:w height:h mipmapped:NO];
    d.usage=MTLTextureUsageRenderTarget; d.storageMode=MTLStorageModePrivate; return [dev newTextureWithDescriptor:d];
}

int main(void){
    @autoreleasepool {
        BincRuntime *rt=binc_runtime_open("build/pong.metallib"); if(!rt)return 1;
        id<MTLDevice> dev=(__bridge id<MTLDevice>)binc_runtime_device(rt);
        id<MTLCommandQueue> queue=(__bridge id<MTLCommandQueue>)binc_runtime_queue(rt);
        id<MTLLibrary> lib=(__bridge id<MTLLibrary>)binc_runtime_library(rt);
        CAMetalLayer *layer=[CAMetalLayer layer]; layer.device=dev; layer.pixelFormat=MTLPixelFormatBGRA8Unorm;
        NSApplication *app=[NSApplication sharedApplication]; [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSRect frame=NSMakeRect(0,0,1024,768);
        NSWindow *win=[[NSWindow alloc] initWithContentRect:frame styleMask:(NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable) backing:NSBackingStoreBuffered defer:NO];
        win.title=@"BinC // NEON PONG";
        PongView *view=[[PongView alloc] initWithFrame:frame]; win.contentView=view; [win makeFirstResponder:view]; [win makeKeyAndOrderFront:nil]; [app activateIgnoringOtherApps:YES];
        layer.frame=view.bounds; [view setWantsLayer:YES]; view.layer=layer; layer.drawableSize=CGSizeMake(1024,768);
        NSError *err=nil;
        MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new]; pd.vertexFunction=[lib newFunctionWithName:@"pong_vs"]; pd.fragmentFunction=[lib newFunctionWithName:@"pong_fs"]; pd.colorAttachments[0].pixelFormat=layer.pixelFormat; pd.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
        id<MTLRenderPipelineState> pipe=[dev newRenderPipelineStateWithDescriptor:pd error:&err]; if(!pipe){ NSLog(@"BinC render pipeline: %@",err); return 1; }
        MTLDepthStencilDescriptor *dd=[MTLDepthStencilDescriptor new]; dd.depthCompareFunction=MTLCompareFunctionLessEqual; dd.depthWriteEnabled=YES; id<MTLDepthStencilState> depth=[dev newDepthStencilStateWithDescriptor:dd];
        HostGame game={0,0,0.1875f,0.5f,0.25f,0,0,0,0,0,30.0f,1,3,0,0,0,0}; float vertices[222*4]={0};
        BincBuffer *gb=binc_buffer_new(rt,&game,sizeof game); BincBuffer *vb=binc_buffer_new(rt,vertices,sizeof vertices);
        NSURL *musicURL=[NSURL fileURLWithPath:@"../examples/pong_music.wav"]; AVAudioPlayer *music=[[AVAudioPlayer alloc] initWithContentsOfURL:musicURL error:&err];
        AVAudioPlayer *hit=[[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:@"../examples/pong_hit.wav"] error:&err];
        AVAudioPlayer *life=[[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:@"../examples/pong_life.wav"] error:&err];
        AVAudioPlayer *score=[[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:@"../examples/pong_score.wav"] error:&err];
        if(music){ music.numberOfLoops=-1; [music setVolume:0.35]; [music prepareToPlay]; [music play]; } else NSLog(@"BinC Pong music unavailable: %@",err);
        if(hit)hit.volume=0.55; if(life)life.volume=0.7; if(score)score.volume=0.7;
        int lastEvent=0; int lastRound=1; int lastLives=3;
        id<MTLTexture> depthTex=nil;
        while(g_running && win.isVisible){ @autoreleasepool {
            NSDate *until=[NSDate dateWithTimeIntervalSinceNow:1.0/120.0]; NSEvent *ev;
            while((ev=[app nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES])){
                if(ev.type==NSEventTypeKeyDown && ev.keyCode==53) g_running=NO;
                [app sendEvent:ev];
            }
            float input=(g_up?1.0f:0.0f)-(g_down?1.0f:0.0f);
            binc_update(rt,1,gb,vb,1.0f/60.0f,input);
            HostGame *state=(HostGame *)binc_buffer_contents(gb);
            int seconds=(int)state->round_time; if(seconds<0)seconds=0;
            win.title=[NSString stringWithFormat:@"BinC // NEON PONG   ROUND %d   TIME %02d   P %d : AI %d   LIVES %d",state->round,seconds,state->player_score,state->ai_score,state->lives];
            if(state->event!=lastEvent){ if(state->event==1){ hit.currentTime=0; [hit play]; } if(state->event==2){ life.currentTime=0; [life play]; } if(state->event==3){ score.currentTime=0; [score play]; } lastEvent=state->event; }
            if(state->round!=lastRound){ lastRound=state->round; }
            if(state->lives!=lastLives){ lastLives=state->lives; }
            id<CAMetalDrawable> drawable=[layer nextDrawable]; if(drawable){
                if(!depthTex || depthTex.width!=(NSUInteger)layer.drawableSize.width || depthTex.height!=(NSUInteger)layer.drawableSize.height) depthTex=makeDepth(dev,(NSUInteger)layer.drawableSize.width,(NSUInteger)layer.drawableSize.height);
                MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture=drawable.texture; rp.colorAttachments[0].loadAction=MTLLoadActionClear; rp.colorAttachments[0].storeAction=MTLStoreActionStore; rp.colorAttachments[0].clearColor=MTLClearColorMake(0.0,0.0,0.01,1.0); rp.depthAttachment.texture=depthTex; rp.depthAttachment.loadAction=MTLLoadActionClear; rp.depthAttachment.storeAction=MTLStoreActionDontCare; rp.depthAttachment.clearDepth=1.0;
                id<MTLCommandBuffer> cb=[queue commandBuffer]; id<MTLRenderCommandEncoder> re=[cb renderCommandEncoderWithDescriptor:rp]; [re setRenderPipelineState:pipe]; [re setDepthStencilState:depth]; [re setVertexBuffer:(__bridge id<MTLBuffer>)binc_buffer_native(vb) offset:0 atIndex:0]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:222]; [re endEncoding]; [cb presentDrawable:drawable]; [cb commit];
            }
        }}
        [music stop]; binc_buffer_release(gb); binc_buffer_release(vb); binc_runtime_close(rt); return 0;
    }
}
