/*
 * mtlprobe - does Metal see the RX 7800 XT through RDNA3MTLDriver, and does
 * a compute kernel run on it?
 *
 *   mtlprobe            every Metal device, then on the AMD one: a kernel
 *                       compiled from source (Apple's front end -> AIR ->
 *                       the plugin's AIR->SPIR-V translator -> RADV), run
 *                       over 4096 threads into a shared buffer, checked;
 *                       then a triangle rendered into a texture, blitted
 *                       into a buffer and checked
 *   mtlprobe --list     the device list only
 *   mtlprobe --iosurface
 *                       A3: an IOSurface as a Metal texture
 *                       (newTextureWithDescriptor:iosurface:plane:): what
 *                       the CPU wrote read by the GPU, what the GPU cleared
 *                       read by the CPU; last line "A3: ..."
 *   mtlprobe --layer [s]
 *                       M4: a window with a CAMetalLayer on the AMD device,
 *                       for s seconds (default 12) a magenta frame drawn,
 *                       read back and presented each 100 ms (run in the
 *                       user's GUI session); last line "M4: ..."
 *
 * The plugin hands out a device only to processes its allow list allows;
 * "mtlprobe" is allowed by name (metal/plugin/NVMTLDevice.m). Its own log goes to
 * NVMTL_LOG_PATH (the report sets it).
 *
 * Last line: "METAL: ALL PASSED ..." or "METAL: FAILED <step> ...".
 * Exit 0 = passed, 1 = a step failed, 2 = no AMD device.
 *
 * SPDX-License-Identifier: MIT
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 4096u

static NSString *const kSrc =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void probe(device uint *o [[buffer(0)]], constant uint &k [[buffer(1)]],\n"
     "                  uint i [[thread_position_in_grid]]) {\n"
     "    o[i] = i * k + 7u;\n"
     "}\n";

static void families(id<MTLDevice> d)
{
    static const struct { MTLGPUFamily f; const char *n; } fam[] = {
        { MTLGPUFamilyCommon1, "Common1" }, { MTLGPUFamilyCommon2, "Common2" },
        { MTLGPUFamilyCommon3, "Common3" }, { MTLGPUFamilyMac2, "Mac2" },
        { MTLGPUFamilyMetal3, "Metal3" },
    };
    printf("  families:");
    for (size_t i = 0; i < sizeof(fam) / sizeof(fam[0]); i++)
        if ([d supportsFamily:fam[i].f])
            printf(" %s", fam[i].n);
    printf("\n");
}

static void describe(unsigned i, id<MTLDevice> d)
{
    printf("device %u: \"%s\" (%s), registry 0x%llx\n", i, d.name.UTF8String,
           NSStringFromClass([(NSObject *)d class]).UTF8String, (unsigned long long)d.registryID);
    printf("  lowPower %d headless %d removable %d unifiedMemory %d, location %lu/%lu\n", d.lowPower,
           d.headless, d.removable, d.hasUnifiedMemory, (unsigned long)d.location,
           (unsigned long)d.locationNumber);
    printf("  recommendedMaxWorkingSetSize %llu MiB, maxBufferLength %llu MiB, maxThreadsPerThreadgroup %lux%lux%lu\n",
           (unsigned long long)(d.recommendedMaxWorkingSetSize >> 20), (unsigned long long)(d.maxBufferLength >> 20),
           (unsigned long)d.maxThreadsPerThreadgroup.width, (unsigned long)d.maxThreadsPerThreadgroup.height,
           (unsigned long)d.maxThreadsPerThreadgroup.depth);
    families(d);
}

static int fail(const char *step, NSError *e)
{
    printf("METAL: FAILED %s%s%s\n", step, e ? ": " : "", e ? e.localizedDescription.UTF8String : "");
    return 1;
}

static int compute(id<MTLDevice> d)
{
    NSError *e = nil;
    printf("== compile (newLibraryWithSource)\n");
    fflush(stdout);
    id<MTLLibrary> lib = [d newLibraryWithSource:kSrc options:nil error:&e];
    if (!lib)
        return fail("newLibraryWithSource", e);
    id<MTLFunction> fn = [lib newFunctionWithName:@"probe"];
    if (!fn)
        return fail("newFunctionWithName(probe)", nil);
    printf("== pipeline (newComputePipelineStateWithFunction)\n");
    fflush(stdout);
    id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:fn error:&e];
    if (!ps)
        return fail("newComputePipelineStateWithFunction", e);
    printf("  maxTotalThreadsPerThreadgroup %lu, threadExecutionWidth %lu\n",
           (unsigned long)ps.maxTotalThreadsPerThreadgroup, (unsigned long)ps.threadExecutionWidth);
    id<MTLBuffer> buf = [d newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    if (!buf)
        return fail("newBufferWithLength", nil);
    memset(buf.contents, 0x5a, N * 4);   /* a kernel that never ran cannot pass */
    id<MTLCommandQueue> q = [d newCommandQueue];
    if (!q)
        return fail("newCommandQueue", nil);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    if (!cb || !ce)
        return fail("commandBuffer/computeCommandEncoder", nil);
    uint32_t k = 3;
    [ce setComputePipelineState:ps];
    [ce setBuffer:buf offset:0 atIndex:0];
    [ce setBytes:&k length:sizeof(k) atIndex:1];
    NSUInteger tg = ps.maxTotalThreadsPerThreadgroup < 64 ? ps.maxTotalThreadsPerThreadgroup : 64;
    [ce dispatchThreadgroups:MTLSizeMake(N / tg, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
    [ce endEncoding];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        (void)c;
        dispatch_semaphore_signal(done);
    }];
    printf("== dispatch %u threads (%lu per group), commit\n", N, (unsigned long)tg);
    fflush(stdout);
    [cb commit];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)))
        return fail("command buffer not completed in 10 s", nil);
    if (cb.status != MTLCommandBufferStatusCompleted) {
        printf("  status %ld\n", (long)cb.status);
        return fail("command buffer status", cb.error);
    }
    const uint32_t *o = buf.contents;
    uint32_t bad = 0, first = N;
    for (uint32_t i = 0; i < N; i++)
        if (o[i] != i * k + 7u) {
            if (first == N)
                first = i;
            bad++;
        }
    if (bad) {
        printf("  %u of %u words wrong, first [%u] = 0x%08x, want 0x%08x\n", bad, N, first, o[first],
               first * k + 7u);
        return fail("kernel result", nil);
    }
    printf("  %u words = i * 3 + 7, GPU time %.3f ms\n", N, (cb.GPUEndTime - cb.GPUStartTime) * 1e3);
    return 0;
}

/* ---- render: a red triangle on a blue clear into a 64x64 RGBA8 texture,
 * blitted into a shared buffer and checked (the Vulkan test's picture) ---- */
#define IMG 64u
static NSString *const kRenderSrc =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "vertex float4 tri_vs(uint v [[vertex_id]]) {\n"
     "    const float2 p[3] = { float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };\n"
     "    return float4(p[v], 0.0, 1.0);\n"
     "}\n"
     "fragment float4 tri_fs() { return float4(1.0, 0.0, 0.0, 1.0); }\n";

static int render(id<MTLDevice> d)
{
    NSError *e = nil;
    printf("== render: compile (vertex + fragment)\n");
    fflush(stdout);
    id<MTLLibrary> lib = [d newLibraryWithSource:kRenderSrc options:nil error:&e];
    if (!lib)
        return fail("render newLibraryWithSource", e);
    MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
    rpd.vertexFunction = [lib newFunctionWithName:@"tri_vs"];
    rpd.fragmentFunction = [lib newFunctionWithName:@"tri_fs"];
    rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (!rpd.vertexFunction || !rpd.fragmentFunction)
        return fail("render newFunctionWithName", nil);
    printf("== render: pipeline (newRenderPipelineStateWithDescriptor)\n");
    fflush(stdout);
    id<MTLRenderPipelineState> ps = [d newRenderPipelineStateWithDescriptor:rpd error:&e];
    if (!ps)
        return fail("newRenderPipelineStateWithDescriptor", e);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:IMG height:IMG mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [d newTextureWithDescriptor:td];
    id<MTLBuffer> out = [d newBufferWithLength:IMG * IMG * 4 options:MTLResourceStorageModeShared];
    if (!tex || !out)
        return fail("render texture/buffer", nil);
    memset(out.contents, 0x5a, IMG * IMG * 4);
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = tex;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
    if (!re)
        return fail("renderCommandEncoderWithDescriptor", nil);
    [re setRenderPipelineState:ps];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re endEncoding];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(IMG, IMG, 1) toBuffer:out destinationOffset:0
      destinationBytesPerRow:IMG * 4 destinationBytesPerImage:IMG * IMG * 4];
    [be endEncoding];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        (void)c;
        dispatch_semaphore_signal(done);
    }];
    printf("== render: draw + blit to a buffer, commit\n");
    fflush(stdout);
    [cb commit];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)))
        return fail("render command buffer not completed in 10 s", nil);
    if (cb.status != MTLCommandBufferStatusCompleted)
        return fail("render command buffer status", cb.error);
    /* Metal's NDC has y up and the texture's row 0 at the top: the vertices
     * land on pixels (0,64) (0,0) (64,0), so red fills the upper-left half,
     * x + y < 63; the pixels on the edge (x + y == 63) may go either way */
    const uint32_t *p = out.contents, red = 0xff0000ff, blue = 0xffff0000;
    uint32_t bad = 0, fx = 0, fy = 0, got = 0, want = 0, nred = 0;
    for (uint32_t y = 0; y < IMG; y++)
        for (uint32_t x = 0; x < IMG; x++) {
            uint32_t v = p[y * IMG + x], w = x + y < IMG - 1 ? red : blue;
            nred += v == red;
            if (x + y == IMG - 1 || v == w)
                continue;
            if (!bad++) { fx = x; fy = y; got = v; want = w; }
        }
    printf("  %u red pixels of %u; %u wrong off the edge", nred, IMG * IMG, bad);
    if (bad)
        printf(", first (%u,%u) = 0x%08x, want 0x%08x (0x5a5a5a5a: the blit never wrote; red in the lower-right: "
               "y flipped)", fx, fy, got, want);
    printf("\n");
    if (bad)
        return fail("render result", nil);
    return 0;
}

/* run 'cb' and wait up to 10 s; 0 when it completed */
static int run_wait(id<MTLCommandBuffer> cb)
{
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        (void)c;
        dispatch_semaphore_signal(done);
    }];
    [cb commit];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)))
        return -1;
    return cb.status == MTLCommandBufferStatusCompleted ? 0 : 1;
}

/* ---- A3: an IOSurface shared between the CPU and the GPU ---- */
#define SURF 256u
static uint32_t surf_px(uint32_t x, uint32_t y)
{
    return 0xff000000u | (x << 16) | (y << 8) | ((x ^ y) & 0xffu);
}

static int iosurface_test(id<MTLDevice> d)
{
    NSDictionary *props = @{ (id)kIOSurfaceWidth: @(SURF), (id)kIOSurfaceHeight: @(SURF),
                             (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    if (!s) {
        printf("A3: FAILED IOSurfaceCreate\n");
        return 1;
    }
    size_t bpr = IOSurfaceGetBytesPerRow(s);
    printf("A3: IOSurface %u, %ux%u BGRA, %zu bytes per row\n", IOSurfaceGetID(s), SURF, SURF, bpr);
    IOSurfaceLock(s, 0, NULL);
    uint8_t *base = IOSurfaceGetBaseAddress(s);
    for (uint32_t y = 0; y < SURF; y++)
        for (uint32_t x = 0; x < SURF; x++)
            ((uint32_t *)(void *)(base + y * bpr))[x] = surf_px(x, y);
    IOSurfaceUnlock(s, 0, NULL);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:SURF height:SURF mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeManaged;
    id<MTLTexture> t = [d newTextureWithDescriptor:td iosurface:s plane:0];
    if (!t) {
        printf("A3: FAILED newTextureWithDescriptor:iosurface:plane: returned nil (the plugin has no IOSurface "
               "textures)\n");
        CFRelease(s);
        return 1;
    }
    printf("A3: texture %s, iosurface %s\n", NSStringFromClass([(NSObject *)t class]).UTF8String,
           t.iosurface ? "kept" : "LOST");
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLBuffer> out = [d newBufferWithLength:SURF * SURF * 4 options:MTLResourceStorageModeShared];
    memset(out.contents, 0x5a, SURF * SURF * 4);
    /* the GPU reads what the CPU wrote */
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(SURF, SURF, 1) toBuffer:out destinationOffset:0
      destinationBytesPerRow:SURF * 4 destinationBytesPerImage:SURF * SURF * 4];
    [be endEncoding];
    int r = run_wait(cb);
    uint32_t good_in = 0, good_out = 0;
    const uint32_t *o = out.contents;
    for (uint32_t y = 0; r == 0 && y < SURF; y++)
        for (uint32_t x = 0; x < SURF; x++)
            good_in += o[y * SURF + x] == surf_px(x, y);
    printf("A3: CPU -> GPU: %s, %u of %u pixels right%s\n", r ? (r < 0 ? "NOT completed in 10 s" : "FAILED") : "done",
           good_in, SURF * SURF, r == 0 && good_in == 0 && o[0] == 0x5a5a5a5au ? " (the blit wrote nothing)" : "");
    /* the CPU reads what the GPU cleared (green) */
    cb = [q commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = t;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
    [re endEncoding];
    be = [cb blitCommandEncoder];
    [be synchronizeResource:t];
    [be endEncoding];
    int r2 = run_wait(cb);
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    base = IOSurfaceGetBaseAddress(s);
    for (uint32_t y = 0; r2 == 0 && y < SURF; y++)
        for (uint32_t x = 0; x < SURF; x++)
            good_out += ((const uint32_t *)(const void *)(base + y * bpr))[x] == 0xff00ff00u;
    uint32_t first = ((const uint32_t *)(const void *)base)[0];
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    printf("A3: GPU -> CPU: %s, %u of %u pixels green, pixel 0 = 0x%08x\n",
           r2 ? (r2 < 0 ? "NOT completed in 10 s" : "FAILED") : "done", good_out, SURF * SURF, first);
    CFRelease(s);
    int ok = good_in == SURF * SURF && good_out == SURF * SURF;
    printf("A3: %s IOSurface texture, CPU -> GPU %u/%u, GPU -> CPU %u/%u\n", ok ? "PASSED" : "FAILED", good_in,
           SURF * SURF, good_out, SURF * SURF);
    return ok ? 0 : 1;
}

/* ---- M4: a CAMetalLayer on the screen ---- */
static int layer_test(id<MTLDevice> d, double seconds)
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 480)
                                              styleMask:NSWindowStyleMaskTitled
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    w.title = @"rdna3 M4 probe";
    [w center];
    CAMetalLayer *l = [CAMetalLayer layer];
    l.device = d;
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.framebufferOnly = NO;
    l.drawableSize = CGSizeMake(640, 480);
    w.contentView.wantsLayer = YES;
    w.contentView.layer = l;
    [w makeKeyAndOrderFront:nil];
    [NSApp activate];   /* macOS 14+: the build targets 15 */
    printf("M4: window %ld on \"%s\" at %.0f,%.0f, layer device %s\n", (long)w.windowNumber,
           w.screen ? w.screen.localizedName.UTF8String : "(no screen)", w.frame.origin.x, w.frame.origin.y,
           l.device ? l.device.name.UTF8String : "(none: the layer refused the device)");
    fflush(stdout);
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLBuffer> px = [d newBufferWithLength:4 options:MTLResourceStorageModeShared];
    __block int presented = 0;
    int frames = 0, nodrawable = 0, completed = 0, magenta = 0;
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([end timeIntervalSinceNow] > 0) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
            frames++;
            id<CAMetalDrawable> dr = [l nextDrawable];
            if (!dr) {
                nodrawable++;
                continue;
            }
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = dr.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
            [re endEncoding];
            *(uint32_t *)px.contents = 0;
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:dr.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(320, 240, 0)
                     sourceSize:MTLSizeMake(1, 1, 1) toBuffer:px destinationOffset:0 destinationBytesPerRow:4
          destinationBytesPerImage:4];
            [be endEncoding];
            [dr addPresentedHandler:^(id<MTLDrawable> x) {
                if (x.presentedTime > 0)
                    presented++;
            }];
            [cb presentDrawable:dr];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status == MTLCommandBufferStatusCompleted)
                completed++;
            magenta += *(uint32_t *)px.contents == 0xffff00ffu;
        }
    }
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.3]];
    printf("M4: %d frames: %d without a drawable, %d completed, %d read back magenta, %d presented on screen\n",
           frames, nodrawable, completed, magenta, presented);
    int ok = completed > 0 && magenta == completed && presented > 0;
    printf("M4: %s CAMetalLayer: drawables %s, rendering %s, presents %s\n", ok ? "PASSED" : "FAILED",
           frames > nodrawable ? "yes" : "NONE", magenta ? "right" : "WRONG", presented ? "shown" : "NEVER shown");
    [w orderOut:nil];
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        int list_only = argc > 1 && !strcmp(argv[1], "--list");
        int surf = argc > 1 && !strcmp(argv[1], "--iosurface");
        int layer = argc > 1 && !strcmp(argv[1], "--layer");
        double secs = argc > 2 ? atof(argv[2]) : 12.0;
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        id<MTLDevice> amd = nil, def = MTLCreateSystemDefaultDevice();
        printf("%lu Metal device(s); default: %s\n", (unsigned long)all.count,
               def ? def.name.UTF8String : "(none)");
        for (unsigned i = 0; i < all.count; i++) {
            describe(i, all[i]);
            if (!amd && ([all[i].name containsString:@"AMD"] || [all[i].name containsString:@"RDNA3MTL"] ||
                         [all[i].name containsString:@"Radeon"]))
                amd = all[i];
        }
        if (list_only)
            return 0;
        if (!amd) {
            printf("%sFAILED no AMD device among %lu (plugin not loaded or refused this process: see "
                   "its log)\n", surf ? "A3: " : layer ? "M4: " : "METAL: ", (unsigned long)all.count);
            return 2;
        }
        printf("== using \"%s\"\n", amd.name.UTF8String);
        if (surf)
            return iosurface_test(amd);
        if (layer)
            return layer_test(amd, secs > 0 ? secs : 12.0);
        if (compute(amd))
            return 1;
        if (render(amd))
            return 1;
        printf("METAL: ALL PASSED device \"%s\", compute kernel and a rendered triangle checked\n",
               amd.name.UTF8String);
        return 0;
    }
}
