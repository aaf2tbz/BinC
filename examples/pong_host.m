/* Thin host for the BinC Pong example. The game update, geometry generation,
 * vertex stage, and fragment stage are all in pong.binc. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "../binc/binc_runtime.h"
#include "../binc/build/pong.h"

static float g_input;
@interface PongView : NSView @end
@implementation PongView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)e { NSString *s=e.charactersIgnoringModifiers.lowercaseString; if([s containsString:@"w"]||[s containsString:@"a"])g_input=1.0f; if([s containsString:@"s"]||[s containsString:@"z"])g_input=-1.0f; }
- (void)keyUp:(NSEvent *)e { (void)e; g_input=0.0f; }
@end

int main(void){
    @autoreleasepool {
        BincRuntime *rt=binc_runtime_open("build/pong.metallib"); if(!rt)return 1;
        id<MTLDevice> dev=(__bridge id<MTLDevice>)binc_runtime_device(rt);
        id<MTLCommandQueue> queue=(__bridge id<MTLCommandQueue>)binc_runtime_queue(rt);
        id<MTLLibrary> lib=(__bridge id<MTLLibrary>)binc_runtime_library(rt);
        CAMetalLayer *layer=[CAMetalLayer layer]; layer.device=dev; layer.pixelFormat=MTLPixelFormatBGRA8Unorm;
        NSApplication *app=[NSApplication sharedApplication]; [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSRect frame=NSMakeRect(0,0,800,600); NSWindow *win=[[NSWindow alloc] initWithContentRect:frame styleMask:(NSWindowStyleMaskTitled|NSWindowStyleMaskClosable) backing:NSBackingStoreBuffered defer:NO];
        PongView *view=[[PongView alloc] initWithFrame:frame]; win.contentView=view; [win makeFirstResponder:view]; [win makeKeyAndOrderFront:nil]; [app activateIgnoringOtherApps:YES];
        layer.frame=view.bounds; [view setWantsLayer:YES]; view.layer=layer; layer.drawableSize=CGSizeMake(800,600);
        NSError *err=nil; MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new]; pd.vertexFunction=[lib newFunctionWithName:@"pong_vs"]; pd.fragmentFunction=[lib newFunctionWithName:@"pong_fs"]; pd.colorAttachments[0].pixelFormat=layer.pixelFormat;
        id<MTLRenderPipelineState> pipe=[dev newRenderPipelineStateWithDescriptor:pd error:&err]; if(!pipe){ NSLog(@"%@",err); return 1; }
        float game[6]={0,0,0.6f,0.35f,0,0}; float vertices[18*4]={0}; BincBuffer *gb=binc_buffer_new(rt,game,sizeof game); BincBuffer *vb=binc_buffer_new(rt,vertices,sizeof vertices);
        for(;;){ @autoreleasepool {
            NSDate *until=[NSDate dateWithTimeIntervalSinceNow:1.0/120.0]; NSEvent *ev;
            while((ev=[app nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES])) [app sendEvent:ev];
            binc_update(rt,1,gb,vb,1.0f/60.0f,g_input);
            id<CAMetalDrawable> drawable=[layer nextDrawable]; if(drawable){ MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture=drawable.texture; rp.colorAttachments[0].loadAction=MTLLoadActionClear; rp.colorAttachments[0].storeAction=MTLStoreActionStore; rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1); id<MTLCommandBuffer> cb=[queue commandBuffer]; id<MTLRenderCommandEncoder> re=[cb renderCommandEncoderWithDescriptor:rp]; [re setRenderPipelineState:pipe]; [re setVertexBuffer:(__bridge id<MTLBuffer>)binc_buffer_native(vb) offset:0 atIndex:0]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:18]; [re endEncoding]; [cb presentDrawable:drawable]; [cb commit]; }
        }}
        return 0;
    }
}
