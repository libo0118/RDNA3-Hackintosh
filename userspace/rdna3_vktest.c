/*
 * rdna3_vktest - Vulkan smoke test straight against the RADV ICD, without
 * the Vulkan loader (the VM has none): dlopen libvulkan_radeon, negotiate
 * the ICD interface, create an instance and a device with one queue per
 * family, then per family fill a device-local buffer, copy it into a
 * host-visible one and check every word on the CPU.
 *
 * What runs where: on the general and compute queues RADV fills and copies
 * 64 KiB with its own compute shaders (ACO-compiled meta shaders: a real
 * dispatch through the GFX/compute ring) and 256 bytes with CP DMA; on a
 * transfer queue it uses SDMA packets.
 *
 *   rdna3_vktest [--loopback] [path/to/libvulkan_radeon.{dylib,so}]
 *
 * --loopback: libdrm_xnu's in-process loopback backend executes SDMA only,
 * so only transfer-only families submit work (tools/radv_host.sh).
 * Exit status: 0 = every tested queue verified, 1 = a failure, 2 = setup.
 *
 * SPDX-License-Identifier: MIT
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

typedef VkResult (VKAPI_PTR *PFN_negotiate)(uint32_t *);
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_icdGipa)(VkInstance, const char *);

static PFN_icdGipa gipa;
static PFN_vkGetDeviceProcAddr gdpa;

#define IFN(inst, name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define DFN(dev, name)  PFN_##name name = (PFN_##name)gdpa(dev, #name)

static const char *vkr(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "MEMORY_MAP_FAILED";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "INCOMPATIBLE_DRIVER";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT";
    default: return "?";
    }
}

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    printf("FAIL %s -> %d (%s)\n", #call, r_, vkr(r_)); return 2; } } while (0)

static int find_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits,
                     VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want &&
            !(mp->memoryTypes[i].propertyFlags & avoid))
            return (int)i;
    return -1;
}

struct ctx {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkPhysicalDeviceMemoryProperties mp;
};

struct buf { VkBuffer b; VkDeviceMemory m; int type; };

static int make_buf(struct ctx *c, struct buf *o, VkDeviceSize size, int host)
{
    DFN(c->dev, vkCreateBuffer);
    DFN(c->dev, vkGetBufferMemoryRequirements);
    DFN(c->dev, vkAllocateMemory);
    DFN(c->dev, vkBindBufferMemory);
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkMemoryRequirements mr;
    CHECK(vkCreateBuffer(c->dev, &bi, NULL, &o->b));
    vkGetBufferMemoryRequirements(c->dev, o->b, &mr);
    if (host)
        o->type = find_type(&c->mp, mr.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    else
        o->type = find_type(&c->mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (o->type < 0)   /* e.g. everything host visible: take any that fits */
        o->type = find_type(&c->mp, mr.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0, 0);
    if (o->type < 0) {
        printf("FAIL no memory type for a %s buffer (bits 0x%x)\n", host ? "host" : "device", mr.memoryTypeBits);
        return 2;
    }
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)o->type };
    CHECK(vkAllocateMemory(c->dev, &ai, NULL, &o->m));
    CHECK(vkBindBufferMemory(c->dev, o->b, o->m, 0));
    return 0;
}

static void free_buf(struct ctx *c, struct buf *o)
{
    DFN(c->dev, vkDestroyBuffer);
    DFN(c->dev, vkFreeMemory);
    if (o->b) vkDestroyBuffer(c->dev, o->b, NULL);
    if (o->m) vkFreeMemory(c->dev, o->m, NULL);
    memset(o, 0, sizeof(*o));
}

/* fill 'size' bytes of a device buffer with 'pat' on queue family 'qf',
 * copy into a host buffer, check */
static int fill_copy(struct ctx *c, uint32_t qf, VkDeviceSize size, uint32_t pat)
{
    DFN(c->dev, vkGetDeviceQueue);
    DFN(c->dev, vkCreateCommandPool);
    DFN(c->dev, vkDestroyCommandPool);
    DFN(c->dev, vkAllocateCommandBuffers);
    DFN(c->dev, vkBeginCommandBuffer);
    DFN(c->dev, vkEndCommandBuffer);
    DFN(c->dev, vkCmdFillBuffer);
    DFN(c->dev, vkCmdCopyBuffer);
    DFN(c->dev, vkCmdPipelineBarrier);
    DFN(c->dev, vkCreateFence);
    DFN(c->dev, vkDestroyFence);
    DFN(c->dev, vkQueueSubmit);
    DFN(c->dev, vkWaitForFences);
    DFN(c->dev, vkMapMemory);
    DFN(c->dev, vkUnmapMemory);
    struct buf dbuf = { 0 }, hbuf = { 0 };
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueue q;
    int ret = 1;

    if (make_buf(c, &dbuf, size, 0) || make_buf(c, &hbuf, size, 1))
        goto out;
    vkGetDeviceQueue(c->dev, qf, 0, &q);
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT };
    VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    VkBufferCopy region = { 0, 0, size };
    VkResult r;
    void *p;

    if ((r = vkCreateCommandPool(c->dev, &pi, NULL, &pool)) != VK_SUCCESS ||
        (cai.commandPool = pool, r = vkAllocateCommandBuffers(c->dev, &cai, &cb)) != VK_SUCCESS ||
        (r = vkCreateFence(c->dev, &fi, NULL, &fence)) != VK_SUCCESS) {
        printf("FAIL pool/command buffer/fence -> %d (%s)\n", r, vkr(r));
        goto out;
    }
    /* the host buffer starts with a different pattern, so a copy that never
     * ran cannot pass */
    if ((r = vkMapMemory(c->dev, hbuf.m, 0, size, 0, &p)) != VK_SUCCESS) {
        printf("FAIL vkMapMemory -> %d (%s)\n", r, vkr(r));
        goto out;
    }
    memset(p, 0x5a, (size_t)size);
    vkUnmapMemory(c->dev, hbuf.m);

    vkBeginCommandBuffer(cb, &bi);
    vkCmdFillBuffer(cb, dbuf.b, 0, size, pat);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL,
                         0, NULL);
    vkCmdCopyBuffer(cb, dbuf.b, hbuf.b, 1, &region);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL,
                         0, NULL);
    if ((r = vkEndCommandBuffer(cb)) != VK_SUCCESS) {
        printf("FAIL vkEndCommandBuffer -> %d (%s)\n", r, vkr(r));
        goto out;
    }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    if ((r = vkQueueSubmit(q, 1, &si, fence)) != VK_SUCCESS) {
        printf("FAIL vkQueueSubmit -> %d (%s)\n", r, vkr(r));
        goto out;
    }
    if ((r = vkWaitForFences(c->dev, 1, &fence, VK_TRUE, 5000000000ull)) != VK_SUCCESS) {
        printf("FAIL fence not signalled in 5 s -> %d (%s)\n", r, vkr(r));
        goto out;
    }
    if ((r = vkMapMemory(c->dev, hbuf.m, 0, size, 0, &p)) != VK_SUCCESS) {
        printf("FAIL vkMapMemory -> %d (%s)\n", r, vkr(r));
        goto out;
    }
    {
        const uint32_t *w = (const uint32_t *)p;
        size_t n = (size_t)size / 4, bad = 0, first = n;
        for (size_t i = 0; i < n; i++)
            if (w[i] != pat) {
                if (first == n) first = i;
                bad++;
            }
        if (bad)
            printf("FAIL %zu of %zu words wrong, first at %zu: 0x%08x (want 0x%08x)\n", bad, n, first, w[first], pat);
        else
            ret = 0;
    }
    vkUnmapMemory(c->dev, hbuf.m);
out:
    if (fence) vkDestroyFence(c->dev, fence, NULL);
    if (pool) vkDestroyCommandPool(c->dev, pool, NULL);
    free_buf(c, &dbuf);
    free_buf(c, &hbuf);
    return ret;
}

int main(int argc, char **argv)
{
    const char *lib = NULL;
    int loopback = 0, failed = 0, tested = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--loopback")) loopback = 1;
        else lib = argv[i];
    }
    if (!lib)
#ifdef __APPLE__
        lib = "libvulkan_radeon.dylib";
#else
        lib = "libvulkan_radeon.so";
#endif
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("FAIL dlopen %s: %s\n", lib, dlerror());
        return 2;
    }
    PFN_negotiate neg = (PFN_negotiate)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    gipa = (PFN_icdGipa)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gipa) {
        printf("FAIL %s exports no vk_icdGetInstanceProcAddr\n", lib);
        return 2;
    }
    uint32_t icdv = 5;
    if (neg && neg(&icdv) != VK_SUCCESS) {
        printf("FAIL ICD interface negotiation\n");
        return 2;
    }
    printf("ICD %s, loader interface %u\n", lib, icdv);

    struct ctx c = { 0 };
    IFN(NULL, vkEnumerateInstanceVersion);
    IFN(NULL, vkCreateInstance);
    uint32_t iv = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&iv);
    printf("instance version %u.%u.%u\n", VK_API_VERSION_MAJOR(iv), VK_API_VERSION_MINOR(iv), VK_API_VERSION_PATCH(iv));
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "rdna3_vktest",
                              .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHECK(vkCreateInstance(&ici, NULL, &c.inst));

    IFN(c.inst, vkEnumeratePhysicalDevices);
    IFN(c.inst, vkGetPhysicalDeviceProperties2);
    IFN(c.inst, vkGetPhysicalDeviceMemoryProperties);
    IFN(c.inst, vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(c.inst, vkCreateDevice);
    IFN(c.inst, vkDestroyInstance);
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(c.inst, "vkGetDeviceProcAddr");
    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(c.inst, &n, NULL));
    printf("physical devices: %u\n", n);
    if (!n) {
        printf("FAIL RADV found no device (run with RADV_DEBUG=startup for the reason)\n");
        return 2;
    }
    n = 1;
    vkEnumeratePhysicalDevices(c.inst, &n, &c.pd);

    VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
    vkGetPhysicalDeviceProperties2(c.pd, &p2);
    printf("device: %s, %04x:%04x, Vulkan %u.%u.%u, driver %s %s\n", p2.properties.deviceName,
           p2.properties.vendorID, p2.properties.deviceID, VK_API_VERSION_MAJOR(p2.properties.apiVersion),
           VK_API_VERSION_MINOR(p2.properties.apiVersion), VK_API_VERSION_PATCH(p2.properties.apiVersion),
           drv.driverName, drv.driverInfo);
    vkGetPhysicalDeviceMemoryProperties(c.pd, &c.mp);
    for (uint32_t i = 0; i < c.mp.memoryHeapCount; i++)
        printf("  heap %u: %llu MiB%s\n", i, (unsigned long long)(c.mp.memoryHeaps[i].size >> 20),
               (c.mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "");
    for (uint32_t i = 0; i < c.mp.memoryTypeCount; i++)
        printf("  type %u: heap %u flags 0x%x\n", i, c.mp.memoryTypes[i].heapIndex,
               c.mp.memoryTypes[i].propertyFlags);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c.pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(c.pd, &nq, qp);
    VkDeviceQueueCreateInfo qci[16];
    float prio = 1.0f;
    for (uint32_t i = 0; i < nq; i++) {
        printf("  queue family %u: flags 0x%x, %u queue(s)\n", i, qp[i].queueFlags, qp[i].queueCount);
        qci[i] = (VkDeviceQueueCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                            .queueFamilyIndex = i, .queueCount = 1, .pQueuePriorities = &prio };
    }
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = nq,
                               .pQueueCreateInfos = qci };
    VkResult r = vkCreateDevice(c.pd, &dci, NULL, &c.dev);
    if (r != VK_SUCCESS) {
        printf("FAIL vkCreateDevice -> %d (%s)\n", r, vkr(r));
        return 2;
    }
    printf("device created\n");

    for (uint32_t i = 0; i < nq; i++) {
        VkQueueFlags f = qp[i].queueFlags;
        int transfer_only = !(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT));
        const char *what = (f & VK_QUEUE_GRAPHICS_BIT) ? "general" : (f & VK_QUEUE_COMPUTE_BIT) ? "compute" : "transfer";
        if (loopback && !transfer_only) {
            printf("queue family %u (%s): skipped (the loopback executes SDMA only)\n", i, what);
            continue;
        }
        /* 64 KiB: RADV's compute-shader fill/copy on GFX/compute queues;
         * 256 B: CP DMA (SDMA either way on a transfer queue) */
        static const VkDeviceSize sizes[2] = { 64 * 1024, 256 };
        for (int s = 0; s < 2; s++) {
            uint32_t pat = 0xc0de0000u | (i << 8) | (uint32_t)s;
            int fr = fill_copy(&c, i, sizes[s], pat);
            printf("%s queue family %u (%s): fill + copy of %llu bytes\n", fr ? "FAIL" : "PASS", i, what,
                   (unsigned long long)sizes[s]);
            failed |= fr != 0;
            tested++;
            if (fr)
                break;   /* a lost device fails every later submit too */
        }
    }
    DFN(c.dev, vkDestroyDevice);
    vkDestroyDevice(c.dev, NULL);
    vkDestroyInstance(c.inst, NULL);
    printf("%s: %d submit(s) tested\n", failed ? "FAILED" : tested ? "ALL PASSED" : "NOTHING TESTED", tested);
    return failed ? 1 : 0;
}
