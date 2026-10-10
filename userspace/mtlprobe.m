/*
 * mtlprobe - does Metal see the RX 7800 XT through RDNA3MTLDriver, and does
 * a compute kernel run on it?
 *
 *   mtlprobe            every Metal device, then on the AMD one: a kernel
 *                       compiled from source (Apple's front end -> AIR ->
 *                       the plugin's AIR->SPIR-V translator -> RADV), run
 *                       over 4096 threads into a shared buffer, checked
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
        printf("METAL: ALL PASSED device \"%s\", compute kernel checked\n", amd.name.UTF8String);
        return 0;
    }
}
