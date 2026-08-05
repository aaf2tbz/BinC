/* Host seam for the 3-D BinC Pong game. Simulation, arena geometry, HUD,
 * vertex stage, fragment atmosphere, rounds, and lives are all in pong.binc. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AVFoundation/AVFoundation.h>
#include "../binc/binc_runtime.h"
#include "../binc/build/pong.h"
#include <string.h>
#include <math.h>
#include <stdint.h>

static BOOL g_up, g_down;
static BOOL g_running=YES;

typedef struct { float ballx,bally,ballz,vx,vy,vz,left,right,trail,clock,round_time; int round,lives,flash,event,player_score,ai_score,game_over; } HostGame;

@interface PongView : NSView
@property(nonatomic,copy) void (^restartHandler)(void);
- (void)restartGame:(id)sender;
@end
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
- (void)restartGame:(id)sender { (void)sender; if(self.restartHandler) self.restartHandler(); }
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
        NSTextField *gameOverLabel=[NSTextField labelWithString:@""]; gameOverLabel.frame=NSMakeRect(0,330,1024,70); gameOverLabel.alignment=NSTextAlignmentCenter; gameOverLabel.font=[NSFont boldSystemFontOfSize:32.0]; gameOverLabel.textColor=[NSColor whiteColor]; gameOverLabel.hidden=YES; [view addSubview:gameOverLabel];
        NSButton *restartButton=[NSButton buttonWithTitle:@"RESTART ROUND" target:view action:@selector(restartGame:)]; restartButton.frame=NSMakeRect(412,285,200,42); restartButton.bezelStyle=NSBezelStyleRounded; restartButton.hidden=YES; [view addSubview:restartButton];
        layer.frame=view.bounds; [view setWantsLayer:YES]; view.layer=layer; layer.drawableSize=CGSizeMake(1024,768);
        NSError *err=nil;
        MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new]; pd.vertexFunction=[lib newFunctionWithName:@"pong_vs"]; pd.fragmentFunction=[lib newFunctionWithName:@"pong_fs"]; pd.colorAttachments[0].pixelFormat=layer.pixelFormat; pd.colorAttachments[0].blendingEnabled=YES; pd.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorSourceAlpha; pd.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha; pd.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorOne; pd.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha; pd.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
        id<MTLRenderPipelineState> pipe=[dev newRenderPipelineStateWithDescriptor:pd error:&err]; if(!pipe){ NSLog(@"BinC render pipeline: %@",err); return 1; }
        MTLDepthStencilDescriptor *dd=[MTLDepthStencilDescriptor new]; dd.depthCompareFunction=MTLCompareFunctionLessEqual; dd.depthWriteEnabled=YES; id<MTLDepthStencilState> depth=[dev newDepthStencilStateWithDescriptor:dd];
        HostGame game={0,0,0.1875f,0.75f,0.375f,0,0,0,0,0,30.0f,1,3,0,0,0,0,0}; HostGame initialGame=game; float vertices[378*4]={0};
        BincBuffer *gb=binc_buffer_new(rt,&game,sizeof game); BincBuffer *vb=binc_buffer_new(rt,vertices,sizeof vertices);
        NSURL *musicURL=[NSURL fileURLWithPath:@"../examples/pong_music.wav"]; AVAudioPlayer *music=[[AVAudioPlayer alloc] initWithContentsOfURL:musicURL error:&err];
        AVAudioPlayer *hit=[[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:@"../examples/pong_hit.wav"] error:&err];
        AVAudioPlayer *life=[[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:@"../examples/pong_life.wav"] error:&err];
        AVAudioPlayer *score=[[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:@"../examples/pong_score.wav"] error:&err];
        if(music){ music.numberOfLoops=-1; music.enableRate=YES; music.rate=1.0; [music setVolume:0.35]; [music prepareToPlay]; [music play]; } else NSLog(@"BinC Pong music unavailable: %@",err);
        if(hit)hit.volume=0.55; if(life)life.volume=0.7; if(score)score.volume=0.7;
        __block BOOL gameOver=NO; __block int lastEvent=0; __block int lastRound=1; __weak PongView *weakView=view;
        view.restartHandler=^{
            memcpy(binc_buffer_contents(gb), &initialGame, sizeof initialGame); memset(binc_buffer_contents(vb),0,sizeof vertices);
            gameOver=NO; lastEvent=0; lastRound=1; music.currentTime=0; music.rate=1.0; music.volume=0.35; [music play]; gameOverLabel.hidden=YES; restartButton.hidden=YES; [weakView.window makeFirstResponder:weakView];
        };
        id<MTLTexture> depthTex=nil;
        while(g_running && win.isVisible){ @autoreleasepool {
            NSDate *until=[NSDate dateWithTimeIntervalSinceNow:1.0/120.0]; NSEvent *ev;
            while((ev=[app nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES])){
                if(ev.type==NSEventTypeKeyDown && ev.keyCode==53) g_running=NO;
                [app sendEvent:ev];
            }
            float input=(g_up?1.0f:0.0f)-(g_down?1.0f:0.0f);
            if(!gameOver) binc_update(rt,1,gb,vb,1.0f/60.0f,input);
            HostGame *state=(HostGame *)binc_buffer_contents(gb);
            int seconds=(int)state->round_time; if(seconds<0)seconds=0;
            win.title=[NSString stringWithFormat:@"BinC // NEON PONG   ROUND %d   TIME %02d   P %d : AI %d   LIVES %d",state->round,seconds,state->player_score,state->ai_score,state->lives];
            if(state->event!=lastEvent){ if(state->event==1){ hit.currentTime=0; [hit play]; } if(state->event==2){ life.currentTime=0; [life play]; } if(state->event==3){ score.currentTime=0; [score play]; } lastEvent=state->event; }
            if(state->round!=lastRound && state->round>0){
                lastRound=state->round; if(music){ music.rate=MIN(1.75,1.0+0.04*(state->round-1)); music.volume=MIN(0.8,0.35+0.025*(state->round-1)); if(music.duration>4){ uint32_t slot=arc4random_uniform(1000); music.currentTime=fmod((double)slot/1000.0*(music.duration-2.0),music.duration-2.0); } [music play]; }
            }
            if(!gameOver && state->game_over){
                gameOver=YES; gameOverLabel.stringValue=state->game_over==2?@"CONGRATS, YOU WON!":@"YOU LOST, TRY AGAIN"; gameOverLabel.hidden=NO; restartButton.hidden=NO;
            }
            id<CAMetalDrawable> drawable=[layer nextDrawable]; if(drawable){
                if(!depthTex || depthTex.width!=(NSUInteger)layer.drawableSize.width || depthTex.height!=(NSUInteger)layer.drawableSize.height) depthTex=makeDepth(dev,(NSUInteger)layer.drawableSize.width,(NSUInteger)layer.drawableSize.height);
                MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture=drawable.texture; rp.colorAttachments[0].loadAction=MTLLoadActionClear; rp.colorAttachments[0].storeAction=MTLStoreActionStore; rp.colorAttachments[0].clearColor=MTLClearColorMake(0.0,0.0,0.01,1.0); rp.depthAttachment.texture=depthTex; rp.depthAttachment.loadAction=MTLLoadActionClear; rp.depthAttachment.storeAction=MTLStoreActionDontCare; rp.depthAttachment.clearDepth=1.0;
                id<MTLCommandBuffer> cb=[queue commandBuffer]; id<MTLRenderCommandEncoder> re=[cb renderCommandEncoderWithDescriptor:rp]; [re setRenderPipelineState:pipe]; [re setDepthStencilState:depth]; [re setVertexBuffer:(__bridge id<MTLBuffer>)binc_buffer_native(vb) offset:0 atIndex:0]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:378]; [re endEncoding]; [cb presentDrawable:drawable]; [cb commit];
            }
        }}
        [music stop]; binc_buffer_release(gb); binc_buffer_release(vb); binc_runtime_close(rt); return 0;
    }
}
