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
 *
 * The plugin hands out a device only to allow-listed processes; "mtlprobe"
 * is allowed by name (metal/plugin/NVMTLDevice.m). Its own log goes to
 * NVMTL_LOG_PATH (the report sets it).
 *
 * Last line: "METAL: ALL PASSED ..." or "METAL: FAILED <step> ...".
 * Exit 0 = passed, 1 = a step failed, 2 = no AMD device.
 *
 * SPDX-License-Identifier: MIT
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
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

int main(int argc, char **argv)
{
    @autoreleasepool {
        int list_only = argc > 1 && !strcmp(argv[1], "--list");
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
            printf("METAL: FAILED no AMD device among %lu (plugin not loaded or refused this process: see "
                   "its log)\n", (unsigned long)all.count);
            return 2;
        }
        printf("== using \"%s\"\n", amd.name.UTF8String);
        if (compute(amd))
            return 1;
        if (render(amd))
            return 1;
        printf("METAL: ALL PASSED device \"%s\", compute kernel and a rendered triangle checked\n",
               amd.name.UTF8String);
        return 0;
    }
}
