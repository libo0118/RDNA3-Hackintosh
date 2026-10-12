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
 * transfer queue (RADV_EXPERIMENTAL=transfer_queue, set here) it uses SDMA
 * packets.
 *
 *   rdna3_vktest [--loopback] [path/to/libvulkan_radeon.{dylib,so}]
 *   rdna3_vktest --wsi [path]   window-system integration only (V3): the
 *                               WSI extensions the ICD offers, and with
 *                               VK_EXT_headless_surface a swapchain whose
 *                               image is cleared and presented; last line
 *                               "WSI: ..."
 *
 * Then on the general queue: a triangle drawn into a 64x64 image through a
 * render pass (graphics pipeline from the SPIR-V below), buffer -> image ->
 * image -> buffer copies of a pattern (optimal tiling), and a timeline
 * semaphore signalled by a submit and waited for on the host.
 *
 * --loopback: libdrm_xnu's in-process loopback backend executes SDMA only:
 * the transfer queue is verified, GFX/compute jobs are only submitted and
 * fenced (RDNA3_LB_GFX_NOP) (tools/radv_host.sh).
 * Exit status: 0 = every tested queue verified, 1 = a failure, 2 = setup.
 *
 * SPDX-License-Identifier: MIT
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* record and run on queue family 'qf': fill 'fill' with 'pat', then (if
 * 'src') copy src -> dst; waits for the fence. 0 = ran. */
static int submit_fill_copy(struct ctx *c, uint32_t qf, VkBuffer fill, VkBuffer src, VkBuffer dst,
                            VkDeviceSize size, uint32_t pat)
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
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueue q;
    VkCommandBuffer cb;
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
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
    int ret = 1;

    vkGetDeviceQueue(c->dev, qf, 0, &q);
    if ((r = vkCreateCommandPool(c->dev, &pi, NULL, &pool)) != VK_SUCCESS ||
        (cai.commandPool = pool, r = vkAllocateCommandBuffers(c->dev, &cai, &cb)) != VK_SUCCESS ||
        (r = vkCreateFence(c->dev, &fi, NULL, &fence)) != VK_SUCCESS) {
        printf("FAIL pool/command buffer/fence -> %d (%s)\n", r, vkr(r));
        goto out;
    }
    vkBeginCommandBuffer(cb, &bi);
    vkCmdFillBuffer(cb, fill, 0, size, pat);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL,
                         0, NULL);
    if (src)
        vkCmdCopyBuffer(cb, src, dst, 1, &region);
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
    ret = 0;
out:
    if (fence) vkDestroyFence(c->dev, fence, NULL);
    if (pool) vkDestroyCommandPool(c->dev, pool, NULL);
    return ret;
}

static int check_words(const void *p, VkDeviceSize size, uint32_t pat)
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
    return bad != 0;
}

/* the host buffer starts with a different pattern, so a copy that never
 * ran cannot pass */
static int host_buf_preset(struct ctx *c, struct buf *h, VkDeviceSize size)
{
    DFN(c->dev, vkMapMemory);
    DFN(c->dev, vkUnmapMemory);
    void *p;
    VkResult r = vkMapMemory(c->dev, h->m, 0, size, 0, &p);
    if (r != VK_SUCCESS) {
        printf("FAIL vkMapMemory -> %d (%s)\n", r, vkr(r));
        return 1;
    }
    memset(p, 0x5a, (size_t)size);
    vkUnmapMemory(c->dev, h->m);
    return 0;
}

static int host_buf_check(struct ctx *c, struct buf *h, VkDeviceSize size, uint32_t pat)
{
    DFN(c->dev, vkMapMemory);
    DFN(c->dev, vkUnmapMemory);
    void *p;
    VkResult r = vkMapMemory(c->dev, h->m, 0, size, 0, &p);
    if (r != VK_SUCCESS) {
        printf("FAIL vkMapMemory -> %d (%s)\n", r, vkr(r));
        return 1;
    }
    int bad = check_words(p, size, pat);
    vkUnmapMemory(c->dev, h->m);
    return bad;
}

/* fill 'size' bytes of a device buffer with 'pat' on queue family 'qf',
 * copy into a host buffer, check */
static int fill_copy(struct ctx *c, uint32_t qf, VkDeviceSize size, uint32_t pat, int verify)
{
    struct buf dbuf = { 0 }, hbuf = { 0 };
    int ret = 1;
    if (make_buf(c, &dbuf, size, 0) || make_buf(c, &hbuf, size, 1) || host_buf_preset(c, &hbuf, size) ||
        submit_fill_copy(c, qf, dbuf.b, dbuf.b, hbuf.b, size, pat))
        goto out;
    ret = verify ? host_buf_check(c, &hbuf, size, pat) : 0;
out:
    free_buf(c, &dbuf);
    free_buf(c, &hbuf);
    return ret;
}

/* VK_EXT_external_memory_host (the Metal plugin imports the caller's pages
 * this way; RADV -> GEM_USERPTR): the GPU fills malloc'd memory directly */
static int host_ptr_test(struct ctx *c, uint32_t qf, VkDeviceSize align, uint32_t pat)
{
    DFN(c->dev, vkGetMemoryHostPointerPropertiesEXT);
    DFN(c->dev, vkCreateBuffer);
    DFN(c->dev, vkGetBufferMemoryRequirements);
    DFN(c->dev, vkAllocateMemory);
    DFN(c->dev, vkBindBufferMemory);
    const VkDeviceSize size = 64 * 1024;
    struct buf b = { 0 };
    void *host = NULL;
    VkResult r;
    int ret = 1;
    if (!vkGetMemoryHostPointerPropertiesEXT || !align || posix_memalign(&host, (size_t)align, (size_t)size)) {
        printf("FAIL no vkGetMemoryHostPointerPropertiesEXT / alignment %llu\n", (unsigned long long)align);
        return 1;
    }
    memset(host, 0x5a, (size_t)size);
    VkMemoryHostPointerPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    if ((r = vkGetMemoryHostPointerPropertiesEXT(c->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                 host, &hp)) != VK_SUCCESS || !hp.memoryTypeBits) {
        printf("FAIL vkGetMemoryHostPointerPropertiesEXT -> %d (%s), types 0x%x\n", r, vkr(r), hp.memoryTypeBits);
        goto out;
    }
    VkExternalMemoryBufferCreateInfo eb = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkMemoryRequirements mr;
    if ((r = vkCreateBuffer(c->dev, &bi, NULL, &b.b)) != VK_SUCCESS) {
        printf("FAIL vkCreateBuffer (host pointer) -> %d (%s)\n", r, vkr(r));
        goto out;
    }
    vkGetBufferMemoryRequirements(c->dev, b.b, &mr);
    uint32_t types = mr.memoryTypeBits & hp.memoryTypeBits;
    int t = -1;
    for (uint32_t i = 0; i < 32 && t < 0; i++)
        if (types & (1u << i)) t = (int)i;
    if (t < 0) {
        printf("FAIL no memory type for the host pointer (buffer 0x%x, pointer 0x%x)\n", mr.memoryTypeBits,
               hp.memoryTypeBits);
        goto out;
    }
    VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = host };
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
        .allocationSize = size, .memoryTypeIndex = (uint32_t)t };
    if ((r = vkAllocateMemory(c->dev, &ai, NULL, &b.m)) != VK_SUCCESS ||
        (r = vkBindBufferMemory(c->dev, b.b, b.m, 0)) != VK_SUCCESS) {
        printf("FAIL import of the host pointer -> %d (%s)\n", r, vkr(r));
        goto out;
    }
    if (submit_fill_copy(c, qf, b.b, VK_NULL_HANDLE, VK_NULL_HANDLE, size, pat))
        goto out;
    ret = check_words(host, size, pat);
out:
    free_buf(c, &b);
    free(host);
    return ret;
}

/* VK_KHR_external_memory_fd (the Metal plugin imports OPAQUE_FD memory;
 * RADV -> PRIME handle <-> fd through drm_xnu): export a device buffer's
 * memory, import it again, fill through the first and read through the
 * second */
static int opaque_fd_test(struct ctx *c, uint32_t qf, uint32_t pat)
{
    DFN(c->dev, vkGetMemoryFdKHR);
    DFN(c->dev, vkCreateBuffer);
    DFN(c->dev, vkGetBufferMemoryRequirements);
    DFN(c->dev, vkAllocateMemory);
    DFN(c->dev, vkBindBufferMemory);
    const VkDeviceSize size = 64 * 1024;
    struct buf a = { 0 }, b = { 0 }, h = { 0 };
    VkResult r;
    int fd = -1, ret = 1;
    if (!vkGetMemoryFdKHR) {
        printf("FAIL no vkGetMemoryFdKHR\n");
        return 1;
    }
    VkExternalMemoryBufferCreateInfo eb = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkMemoryRequirements mr;
    if ((r = vkCreateBuffer(c->dev, &bi, NULL, &a.b)) != VK_SUCCESS ||
        (r = vkCreateBuffer(c->dev, &bi, NULL, &b.b)) != VK_SUCCESS) {
        printf("FAIL vkCreateBuffer (external) -> %d (%s)\n", r, vkr(r));
        goto out;
    }
    vkGetBufferMemoryRequirements(c->dev, a.b, &mr);
    a.type = find_type(&c->mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkExportMemoryAllocateInfo ex = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ex,
        .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)(a.type < 0 ? 0 : a.type) };
    if ((r = vkAllocateMemory(c->dev, &ai, NULL, &a.m)) != VK_SUCCESS ||
        (r = vkBindBufferMemory(c->dev, a.b, a.m, 0)) != VK_SUCCESS) {
        printf("FAIL exportable allocation -> %d (%s)\n", r, vkr(r));
        goto out;
    }
    VkMemoryGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = a.m,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    if ((r = vkGetMemoryFdKHR(c->dev, &gi, &fd)) != VK_SUCCESS) {
        printf("FAIL vkGetMemoryFdKHR -> %d (%s)\n", r, vkr(r));
        goto out;
    }
    VkImportMemoryFdInfoKHR imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = fd };
    ai.pNext = &imp;
    if ((r = vkAllocateMemory(c->dev, &ai, NULL, &b.m)) != VK_SUCCESS) {
        printf("FAIL import of the exported fd %d -> %d (%s)\n", fd, r, vkr(r));
        goto out;
    }
    fd = -1;   /* the import owns it now */
    if ((r = vkBindBufferMemory(c->dev, b.b, b.m, 0)) != VK_SUCCESS) {
        printf("FAIL vkBindBufferMemory (imported) -> %d (%s)\n", r, vkr(r));
        goto out;
    }
    if (make_buf(c, &h, size, 1) || host_buf_preset(c, &h, size) ||
        submit_fill_copy(c, qf, a.b, b.b, h.b, size, pat))
        goto out;
    ret = host_buf_check(c, &h, size, pat);
out:
    if (fd >= 0)
        close(fd);
    free_buf(c, &a);
    free_buf(c, &b);
    free_buf(c, &h);
    return ret;
}

/* ---- graphics: a triangle into an image ----
 *
 * vertex (glslangValidator -V):
 *   #version 450
 *   void main() {
 *       const vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
 *       gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
 *   }
 * fragment:
 *   #version 450
 *   layout(location = 0) out vec4 color;
 *   void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }
 */
static const uint32_t tri_vert_spv[] = {
    0x07230203,0x00010000,0x0008000b,0x00000027,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000d,0x0000001a,0x00030003,
    0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00060005,0x0000000b,
    0x505f6c67,0x65567265,0x78657472,0x00000000,0x00060006,0x0000000b,0x00000000,0x505f6c67,
    0x7469736f,0x006e6f69,0x00070006,0x0000000b,0x00000001,0x505f6c67,0x746e696f,0x657a6953,
    0x00000000,0x00070006,0x0000000b,0x00000002,0x435f6c67,0x4470696c,0x61747369,0x0065636e,
    0x00070006,0x0000000b,0x00000003,0x435f6c67,0x446c6c75,0x61747369,0x0065636e,0x00030005,
    0x0000000d,0x00000000,0x00060005,0x0000001a,0x565f6c67,0x65747265,0x646e4978,0x00007865,
    0x00050005,0x0000001d,0x65646e69,0x6c626178,0x00000065,0x00030047,0x0000000b,0x00000002,
    0x00050048,0x0000000b,0x00000000,0x0000000b,0x00000000,0x00050048,0x0000000b,0x00000001,
    0x0000000b,0x00000001,0x00050048,0x0000000b,0x00000002,0x0000000b,0x00000003,0x00050048,
    0x0000000b,0x00000003,0x0000000b,0x00000004,0x00040047,0x0000001a,0x0000000b,0x0000002a,
    0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,
    0x00040017,0x00000007,0x00000006,0x00000004,0x00040015,0x00000008,0x00000020,0x00000000,
    0x0004002b,0x00000008,0x00000009,0x00000001,0x0004001c,0x0000000a,0x00000006,0x00000009,
    0x0006001e,0x0000000b,0x00000007,0x00000006,0x0000000a,0x0000000a,0x00040020,0x0000000c,
    0x00000003,0x0000000b,0x0004003b,0x0000000c,0x0000000d,0x00000003,0x00040015,0x0000000e,
    0x00000020,0x00000001,0x0004002b,0x0000000e,0x0000000f,0x00000000,0x00040017,0x00000010,
    0x00000006,0x00000002,0x0004002b,0x00000008,0x00000011,0x00000003,0x0004001c,0x00000012,
    0x00000010,0x00000011,0x0004002b,0x00000006,0x00000013,0xbf800000,0x0005002c,0x00000010,
    0x00000014,0x00000013,0x00000013,0x0004002b,0x00000006,0x00000015,0x3f800000,0x0005002c,
    0x00000010,0x00000016,0x00000013,0x00000015,0x0005002c,0x00000010,0x00000017,0x00000015,
    0x00000015,0x0006002c,0x00000012,0x00000018,0x00000014,0x00000016,0x00000017,0x00040020,
    0x00000019,0x00000001,0x0000000e,0x0004003b,0x00000019,0x0000001a,0x00000001,0x00040020,
    0x0000001c,0x00000007,0x00000012,0x00040020,0x0000001e,0x00000007,0x00000010,0x0004002b,
    0x00000006,0x00000021,0x00000000,0x00040020,0x00000025,0x00000003,0x00000007,0x00050036,
    0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003b,0x0000001c,
    0x0000001d,0x00000007,0x0004003d,0x0000000e,0x0000001b,0x0000001a,0x0003003e,0x0000001d,
    0x00000018,0x00050041,0x0000001e,0x0000001f,0x0000001d,0x0000001b,0x0004003d,0x00000010,
    0x00000020,0x0000001f,0x00050051,0x00000006,0x00000022,0x00000020,0x00000000,0x00050051,
    0x00000006,0x00000023,0x00000020,0x00000001,0x00070050,0x00000007,0x00000024,0x00000022,
    0x00000023,0x00000021,0x00000015,0x00050041,0x00000025,0x00000026,0x0000000d,0x0000000f,
    0x0003003e,0x00000026,0x00000024,0x000100fd,0x00010038
};
static const uint32_t tri_frag_spv[] = {
    0x07230203,0x00010000,0x0008000b,0x0000000d,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0006000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x00030010,0x00000004,
    0x00000007,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,
    0x00040005,0x00000009,0x6f6c6f63,0x00000072,0x00040047,0x00000009,0x0000001e,0x00000000,
    0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,
    0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,0x00000003,0x00000007,
    0x0004003b,0x00000008,0x00000009,0x00000003,0x0004002b,0x00000006,0x0000000a,0x3f800000,
    0x0004002b,0x00000006,0x0000000b,0x00000000,0x0007002c,0x00000007,0x0000000c,0x0000000a,
    0x0000000b,0x0000000b,0x0000000a,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,
    0x000200f8,0x00000005,0x0003003e,0x00000009,0x0000000c,0x000100fd,0x00010038
};

#define IMG 64u
#define RGBA(r, g, b, a) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | (uint32_t)(a) << 24)

struct img { VkImage i; VkDeviceMemory m; VkImageView v; };

static int make_img(struct ctx *c, struct img *o, VkImageUsageFlags usage, int view)
{
    DFN(c->dev, vkCreateImage);
    DFN(c->dev, vkGetImageMemoryRequirements);
    DFN(c->dev, vkAllocateMemory);
    DFN(c->dev, vkBindImageMemory);
    DFN(c->dev, vkCreateImageView);
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { IMG, IMG, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements mr;
    CHECK(vkCreateImage(c->dev, &ii, NULL, &o->i));
    vkGetImageMemoryRequirements(c->dev, o->i, &mr);
    int t = find_type(&c->mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (t < 0) {
        printf("FAIL no memory type for an image (bits 0x%x)\n", mr.memoryTypeBits);
        return 2;
    }
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = (uint32_t)t };
    CHECK(vkAllocateMemory(c->dev, &ai, NULL, &o->m));
    CHECK(vkBindImageMemory(c->dev, o->i, o->m, 0));
    if (view) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->i,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        CHECK(vkCreateImageView(c->dev, &vi, NULL, &o->v));
    }
    return 0;
}

static void free_img(struct ctx *c, struct img *o)
{
    DFN(c->dev, vkDestroyImageView);
    DFN(c->dev, vkDestroyImage);
    DFN(c->dev, vkFreeMemory);
    if (o->v) vkDestroyImageView(c->dev, o->v, NULL);
    if (o->i) vkDestroyImage(c->dev, o->i, NULL);
    if (o->m) vkFreeMemory(c->dev, o->m, NULL);
    memset(o, 0, sizeof(*o));
}

/* one primary command buffer on queue family qf, recorded by the caller
 * between rec_begin and rec_submit (which waits for the fence) */
struct rec { VkCommandPool pool; VkCommandBuffer cb; VkFence fence; VkQueue q; };

static int rec_begin(struct ctx *c, struct rec *r, uint32_t qf)
{
    DFN(c->dev, vkGetDeviceQueue);
    DFN(c->dev, vkCreateCommandPool);
    DFN(c->dev, vkAllocateCommandBuffers);
    DFN(c->dev, vkBeginCommandBuffer);
    DFN(c->dev, vkCreateFence);
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    memset(r, 0, sizeof(*r));
    vkGetDeviceQueue(c->dev, qf, 0, &r->q);
    CHECK(vkCreateCommandPool(c->dev, &pi, NULL, &r->pool));
    cai.commandPool = r->pool;
    CHECK(vkAllocateCommandBuffers(c->dev, &cai, &r->cb));
    CHECK(vkCreateFence(c->dev, &fi, NULL, &r->fence));
    CHECK(vkBeginCommandBuffer(r->cb, &bi));
    return 0;
}

static int rec_submit(struct ctx *c, struct rec *r, const void *pnext, VkSemaphore signal)
{
    DFN(c->dev, vkEndCommandBuffer);
    DFN(c->dev, vkQueueSubmit);
    DFN(c->dev, vkWaitForFences);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = pnext, .commandBufferCount = 1,
        .pCommandBuffers = &r->cb, .signalSemaphoreCount = signal ? 1 : 0, .pSignalSemaphores = &signal };
    VkResult res;
    CHECK(vkEndCommandBuffer(r->cb));
    if ((res = vkQueueSubmit(r->q, 1, &si, r->fence)) != VK_SUCCESS) {
        printf("FAIL vkQueueSubmit -> %d (%s)\n", res, vkr(res));
        return 1;
    }
    if ((res = vkWaitForFences(c->dev, 1, &r->fence, VK_TRUE, 5000000000ull)) != VK_SUCCESS) {
        printf("FAIL fence not signalled in 5 s -> %d (%s)\n", res, vkr(res));
        return 1;
    }
    return 0;
}

static void rec_free(struct ctx *c, struct rec *r)
{
    DFN(c->dev, vkDestroyFence);
    DFN(c->dev, vkDestroyCommandPool);
    if (r->fence) vkDestroyFence(c->dev, r->fence, NULL);
    if (r->pool) vkDestroyCommandPool(c->dev, r->pool, NULL);
    memset(r, 0, sizeof(*r));
}

static void img_barrier(struct ctx *c, VkCommandBuffer cb, VkImage i, VkImageLayout from, VkImageLayout to,
                        VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    DFN(c->dev, vkCmdPipelineBarrier);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src,
        .dstAccessMask = dst, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = i, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

/* map the 64x64 RGBA host buffer and compare pixels; NULL want = see 'fn' */
static int check_pixels(struct ctx *c, struct buf *h, uint32_t (*fn)(uint32_t x, uint32_t y), const char *what)
{
    DFN(c->dev, vkMapMemory);
    DFN(c->dev, vkUnmapMemory);
    uint32_t *p, bad = 0, fx = 0, fy = 0, got = 0;
    VkResult r = vkMapMemory(c->dev, h->m, 0, IMG * IMG * 4, 0, (void **)&p);
    if (r != VK_SUCCESS) {
        printf("FAIL vkMapMemory -> %d (%s)\n", r, vkr(r));
        return 1;
    }
    for (uint32_t y = 0; y < IMG; y++)
        for (uint32_t x = 0; x < IMG; x++)
            if (p[y * IMG + x] != fn(x, y)) {
                if (!bad) { fx = x; fy = y; got = p[y * IMG + x]; }
                bad++;
            }
    vkUnmapMemory(c->dev, h->m);
    if (bad)
        printf("FAIL %s: %u of %u pixels wrong, first (%u,%u) = 0x%08x, want 0x%08x\n", what, bad, IMG * IMG,
               fx, fy, got, fn(fx, fy));
    return bad != 0;
}

/* the triangle covers pixels below the top-left/bottom-right diagonal; the
 * diagonal itself depends on the rasterizer's tie rule: accept both */
static uint32_t tri_px(uint32_t x, uint32_t y)
{
    return y > x ? RGBA(255, 0, 0, 255) : RGBA(0, 0, 255, 255);
}

static int draw_test(struct ctx *c, uint32_t qf, int verify)
{
    DFN(c->dev, vkCreateShaderModule);
    DFN(c->dev, vkDestroyShaderModule);
    DFN(c->dev, vkCreateRenderPass);
    DFN(c->dev, vkDestroyRenderPass);
    DFN(c->dev, vkCreateFramebuffer);
    DFN(c->dev, vkDestroyFramebuffer);
    DFN(c->dev, vkCreatePipelineLayout);
    DFN(c->dev, vkDestroyPipelineLayout);
    DFN(c->dev, vkCreateGraphicsPipelines);
    DFN(c->dev, vkDestroyPipeline);
    DFN(c->dev, vkCmdBeginRenderPass);
    DFN(c->dev, vkCmdEndRenderPass);
    DFN(c->dev, vkCmdBindPipeline);
    DFN(c->dev, vkCmdDraw);
    DFN(c->dev, vkCmdCopyImageToBuffer);
    DFN(c->dev, vkCmdPipelineBarrier);
    struct img im = { 0 };
    struct buf hb = { 0 };
    struct rec r = { 0 };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult res;
    int ret = 1;

    if (make_img(c, &im, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1) ||
        make_buf(c, &hb, IMG * IMG * 4, 1) || host_buf_preset(c, &hb, IMG * IMG * 4))
        goto out;
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(tri_vert_spv), .pCode = tri_vert_spv };
    if ((res = vkCreateShaderModule(c->dev, &smi, NULL, &vs)) != VK_SUCCESS)
        goto vkfail;
    smi.codeSize = sizeof(tri_frag_spv);
    smi.pCode = tri_frag_spv;
    if ((res = vkCreateShaderModule(c->dev, &smi, NULL, &fs)) != VK_SUCCESS)
        goto vkfail;
    VkAttachmentDescription ad = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ar };
    VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd, .dependencyCount = 1, .pDependencies = &dep };
    if ((res = vkCreateRenderPass(c->dev, &rpi, NULL, &rp)) != VK_SUCCESS)
        goto vkfail;
    VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
        .attachmentCount = 1, .pAttachments = &im.v, .width = IMG, .height = IMG, .layers = 1 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    if ((res = vkCreateFramebuffer(c->dev, &fbi, NULL, &fb)) != VK_SUCCESS ||
        (res = vkCreatePipelineLayout(c->dev, &pli, NULL, &pl)) != VK_SUCCESS)
        goto vkfail;
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, IMG, IMG, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { IMG, IMG } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
        .layout = pl, .renderPass = rp, .subpass = 0 };
    if ((res = vkCreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe)) != VK_SUCCESS)
        goto vkfail;
    if (rec_begin(c, &r, qf))
        goto out;
    VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } } };
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
        .framebuffer = fb, .renderArea = sc, .clearValueCount = 1, .pClearValues = &clear };
    vkCmdBeginRenderPass(r.cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(r.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(r.cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(r.cb);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { IMG, IMG, 1 } };
    vkCmdCopyImageToBuffer(r.cb, im.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hb.b, 1, &bic);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(r.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (rec_submit(c, &r, NULL, VK_NULL_HANDLE))
        goto out;
    if (!verify) {
        ret = 0;
        goto out;
    }
    {
        /* the diagonal pixels may go either way: compare everything else */
        DFN(c->dev, vkMapMemory);
        DFN(c->dev, vkUnmapMemory);
        uint32_t *p, bad = 0;
        if ((res = vkMapMemory(c->dev, hb.m, 0, IMG * IMG * 4, 0, (void **)&p)) != VK_SUCCESS)
            goto vkfail;
        for (uint32_t y = 0; y < IMG; y++)
            for (uint32_t x = 0; x < IMG; x++)
                if (x != y && p[y * IMG + x] != tri_px(x, y)) {
                    if (!bad)
                        printf("FAIL triangle: pixel (%u,%u) = 0x%08x, want 0x%08x\n", x, y, p[y * IMG + x],
                               tri_px(x, y));
                    bad++;
                }
        vkUnmapMemory(c->dev, hb.m);
        if (bad)
            printf("FAIL triangle: %u of %u pixels wrong\n", bad, IMG * IMG - IMG);
        ret = bad != 0;
    }
    goto out;
vkfail:
    printf("FAIL draw setup -> %d (%s)\n", res, vkr(res));
out:
    rec_free(c, &r);
    if (pipe) vkDestroyPipeline(c->dev, pipe, NULL);
    if (pl) vkDestroyPipelineLayout(c->dev, pl, NULL);
    if (fb) vkDestroyFramebuffer(c->dev, fb, NULL);
    if (rp) vkDestroyRenderPass(c->dev, rp, NULL);
    if (fs) vkDestroyShaderModule(c->dev, fs, NULL);
    if (vs) vkDestroyShaderModule(c->dev, vs, NULL);
    free_img(c, &im);
    free_buf(c, &hb);
    return ret;
}

/* ---- images: buffer -> image -> image -> buffer of a pattern ---- */

static uint32_t pat_px(uint32_t x, uint32_t y)
{
    return RGBA(x * 4, y * 4, (x ^ y) * 4, 0x80 + (x + y) % 64);
}

static int image_copy_test(struct ctx *c, uint32_t qf, int verify)
{
    DFN(c->dev, vkMapMemory);
    DFN(c->dev, vkUnmapMemory);
    DFN(c->dev, vkCmdCopyBufferToImage);
    DFN(c->dev, vkCmdCopyImage);
    DFN(c->dev, vkCmdCopyImageToBuffer);
    DFN(c->dev, vkCmdPipelineBarrier);
    const VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    struct img a = { 0 }, b = { 0 };
    struct buf src = { 0 }, dst = { 0 };
    struct rec r = { 0 };
    uint32_t *p;
    VkResult res;
    int ret = 1;
    if (make_img(c, &a, u, 0) || make_img(c, &b, u, 0) || make_buf(c, &src, IMG * IMG * 4, 1) ||
        make_buf(c, &dst, IMG * IMG * 4, 1) || host_buf_preset(c, &dst, IMG * IMG * 4))
        goto out;
    if ((res = vkMapMemory(c->dev, src.m, 0, IMG * IMG * 4, 0, (void **)&p)) != VK_SUCCESS) {
        printf("FAIL vkMapMemory -> %d (%s)\n", res, vkr(res));
        goto out;
    }
    for (uint32_t y = 0; y < IMG; y++)
        for (uint32_t x = 0; x < IMG; x++)
            p[y * IMG + x] = pat_px(x, y);
    vkUnmapMemory(c->dev, src.m);
    if (rec_begin(c, &r, qf))
        goto out;
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { IMG, IMG, 1 } };
    VkImageCopy ic = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = { IMG, IMG, 1 } };
    img_barrier(c, r.cb, a.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    img_barrier(c, r.cb, b.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(r.cb, src.b, a.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    img_barrier(c, r.cb, a.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyImage(r.cb, a.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    img_barrier(c, r.cb, b.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyImageToBuffer(r.cb, b.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.b, 1, &bic);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(r.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (rec_submit(c, &r, NULL, VK_NULL_HANDLE))
        goto out;
    ret = verify ? check_pixels(c, &dst, pat_px, "image copies") : 0;
out:
    rec_free(c, &r);
    free_img(c, &a);
    free_img(c, &b);
    free_buf(c, &src);
    free_buf(c, &dst);
    return ret;
}

/* ---- a timeline semaphore signalled by a submit, waited for on the host ---- */

static int timeline_test(struct ctx *c, uint32_t qf)
{
    DFN(c->dev, vkCreateSemaphore);
    DFN(c->dev, vkDestroySemaphore);
    DFN(c->dev, vkWaitSemaphores);
    DFN(c->dev, vkGetSemaphoreCounterValue);
    VkSemaphoreTypeCreateInfo ti = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 1 };
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti };
    VkSemaphore sem = VK_NULL_HANDLE;
    struct rec r = { 0 };
    uint64_t sig = 7, now = 0;
    VkResult res;
    int ret = 1;
    if (!vkWaitSemaphores || !vkGetSemaphoreCounterValue) {
        printf("FAIL no vkWaitSemaphores\n");
        return 1;
    }
    if ((res = vkCreateSemaphore(c->dev, &sci, NULL, &sem)) != VK_SUCCESS) {
        printf("FAIL timeline semaphore -> %d (%s)\n", res, vkr(res));
        return 1;
    }
    VkTimelineSemaphoreSubmitInfo tsi = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sig };
    if (rec_begin(c, &r, qf) || rec_submit(c, &r, &tsi, sem))
        goto out;
    VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
        .pSemaphores = &sem, .pValues = &sig };
    if ((res = vkWaitSemaphores(c->dev, &wi, 5000000000ull)) != VK_SUCCESS) {
        printf("FAIL vkWaitSemaphores(7) -> %d (%s)\n", res, vkr(res));
        goto out;
    }
    vkGetSemaphoreCounterValue(c->dev, sem, &now);
    if (now != 7) {
        printf("FAIL timeline value %llu, want 7\n", (unsigned long long)now);
        goto out;
    }
    ret = 0;
out:
    rec_free(c, &r);
    if (sem) vkDestroySemaphore(c->dev, sem, NULL);
    return ret;
}

/* ---- a descriptor set layout with a mutable binding, bindings not in
 * binding-number order (as the Metal plugin declares them): the type list
 * pMutableDescriptorTypeLists[i] belongs to pBindings[i], whatever order
 * the driver sorts them into. Host-side only, no GPU work. ---- */
static int mutable_layout_test(struct ctx *c)
{
    DFN(c->dev, vkCreateDescriptorSetLayout);
    DFN(c->dev, vkDestroyDescriptorSetLayout);
    static const VkDescriptorType tex[2] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER };
    /* sorted by number the mutable binding moves from index 0 to 1 */
    VkDescriptorSetLayoutBinding b[3] = {
        { .binding = 480, .descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_EXT, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_ALL },
        { .binding = 640, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_ALL },
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_ALL } };
    VkMutableDescriptorTypeListEXT ml[3] = { { 2, tex }, { 0, NULL }, { 0, NULL } };
    VkMutableDescriptorTypeCreateInfoEXT mci = { .sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,
        .mutableDescriptorTypeListCount = 3, .pMutableDescriptorTypeLists = ml };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &mci,
        .bindingCount = 3, .pBindings = b };
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    VkResult r = vkCreateDescriptorSetLayout(c->dev, &lci, NULL, &l);
    if (r != VK_SUCCESS) {
        printf("FAIL mutable layout -> %d (%s)\n", r, vkr(r));
        return 1;
    }
    vkDestroyDescriptorSetLayout(c->dev, l, NULL);
    return 0;
}

/* ---- V3: what RADV offers for presenting (no window system here: a
 * headless surface when it has one, the names of the others) ---- */
static int has_ext(const VkExtensionProperties *ep, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ep[i].extensionName, name))
            return 1;
    return 0;
}

static int wsi_test(void)
{
    static const char *const inst_wsi[] = { "VK_KHR_surface", "VK_EXT_headless_surface", "VK_EXT_metal_surface",
                                            "VK_MVK_macos_surface", "VK_KHR_display", "VK_EXT_direct_mode_display",
                                            "VK_KHR_get_surface_capabilities2" };
    IFN(NULL, vkEnumerateInstanceExtensionProperties);
    IFN(NULL, vkCreateInstance);
    uint32_t n = 0, i;
    VkExtensionProperties ie[256];
    if (!vkEnumerateInstanceExtensionProperties) {
        printf("WSI: FAILED no vkEnumerateInstanceExtensionProperties\n");
        return 2;
    }
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    if (n > 256) n = 256;
    vkEnumerateInstanceExtensionProperties(NULL, &n, ie);
    printf("instance extensions (%u):", n);
    for (i = 0; i < n; i++)
        printf(" %s", ie[i].extensionName);
    printf("\n");
    for (i = 0; i < sizeof(inst_wsi) / sizeof(inst_wsi[0]); i++)
        printf("  %-34s %s\n", inst_wsi[i], has_ext(ie, n, inst_wsi[i]) ? "yes" : "no");
    int headless = has_ext(ie, n, "VK_EXT_headless_surface") && has_ext(ie, n, "VK_KHR_surface");
    int metal = has_ext(ie, n, "VK_EXT_metal_surface");
    const char *want[2] = { "VK_KHR_surface", "VK_EXT_headless_surface" };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "rdna3_vktest wsi",
                              .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                 .enabledExtensionCount = headless ? 2 : 0, .ppEnabledExtensionNames = want };
    struct ctx c = { 0 };
    CHECK(vkCreateInstance(&ici, NULL, &c.inst));
    IFN(c.inst, vkEnumeratePhysicalDevices);
    IFN(c.inst, vkEnumerateDeviceExtensionProperties);
    IFN(c.inst, vkCreateDevice);
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(c.inst, "vkGetDeviceProcAddr");
    n = 1;
    if (vkEnumeratePhysicalDevices(c.inst, &n, &c.pd) != VK_SUCCESS || !n) {
        printf("WSI: FAILED no physical device\n");
        return 2;
    }
    VkExtensionProperties de[512];
    uint32_t nd = 0;
    vkEnumerateDeviceExtensionProperties(c.pd, NULL, &nd, NULL);
    if (nd > 512) nd = 512;
    vkEnumerateDeviceExtensionProperties(c.pd, NULL, &nd, de);
    int swapchain = has_ext(de, nd, "VK_KHR_swapchain");
    printf("  %-34s %s\n  %-34s %s\n", "VK_KHR_swapchain (device)", swapchain ? "yes" : "no",
           "VK_EXT_swapchain_maintenance1", has_ext(de, nd, "VK_EXT_swapchain_maintenance1") ? "yes" : "no");
    if (!headless || !swapchain) {
        printf("WSI: no present path to try: headless surface %s, swapchain %s, Metal surface %s\n",
               headless ? "yes" : "no", swapchain ? "yes" : "no", metal ? "yes" : "no");
        return 1;
    }
    /* a headless surface, a swapchain of 2, one image cleared and presented */
    PFN_vkCreateHeadlessSurfaceEXT vkCreateHeadlessSurfaceEXT =
        (PFN_vkCreateHeadlessSurfaceEXT)gipa(c.inst, "vkCreateHeadlessSurfaceEXT");
    IFN(c.inst, vkGetPhysicalDeviceSurfaceSupportKHR);
    IFN(c.inst, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    IFN(c.inst, vkGetPhysicalDeviceSurfaceFormatsKHR);
    VkHeadlessSurfaceCreateInfoEXT hci = { .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
    VkSurfaceKHR surf;
    CHECK(vkCreateHeadlessSurfaceEXT(c.inst, &hci, NULL, &surf));
    VkBool32 sup = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(c.pd, 0, surf, &sup);
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c.pd, surf, &caps));
    VkSurfaceFormatKHR fm[32];
    uint32_t nf = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(c.pd, surf, &nf, fm);
    printf("headless surface: queue family 0 %s, images %u..%u, %u format(s), first %d\n",
           sup ? "can present" : "CANNOT present", caps.minImageCount, caps.maxImageCount, nf, nf ? (int)fm[0].format : -1);
    if (!sup || !nf) {
        printf("WSI: FAILED the headless surface takes no presents from queue family 0\n");
        return 1;
    }
    const char *dext = "VK_KHR_swapchain";
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &dext };
    CHECK(vkCreateDevice(c.pd, &dci, NULL, &c.dev));
    DFN(c.dev, vkCreateSwapchainKHR);
    DFN(c.dev, vkGetSwapchainImagesKHR);
    DFN(c.dev, vkAcquireNextImageKHR);
    DFN(c.dev, vkQueuePresentKHR);
    DFN(c.dev, vkCreateFence);
    DFN(c.dev, vkWaitForFences);
    DFN(c.dev, vkCmdClearColorImage);
    VkExtent2D ext = caps.currentExtent.width == 0xffffffffu ? (VkExtent2D){ 256, 256 } : caps.currentExtent;
    VkSwapchainCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf,
        .minImageCount = caps.minImageCount > 2 ? caps.minImageCount : 2, .imageFormat = fm[0].format,
        .imageColorSpace = fm[0].colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    VkSwapchainKHR sc;
    CHECK(vkCreateSwapchainKHR(c.dev, &sci, NULL, &sc));
    VkImage imgs[8];
    uint32_t ni = 8, idx = 0;
    CHECK(vkGetSwapchainImagesKHR(c.dev, sc, &ni, imgs));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence acq;
    CHECK(vkCreateFence(c.dev, &fi, NULL, &acq));
    CHECK(vkAcquireNextImageKHR(c.dev, sc, 5000000000ull, VK_NULL_HANDLE, acq, &idx));
    CHECK(vkWaitForFences(c.dev, 1, &acq, VK_TRUE, 5000000000ull));
    struct rec r;
    if (rec_begin(&c, &r, 0))
        return 2;
    VkClearColorValue magenta = { .float32 = { 1.0f, 0.0f, 1.0f, 1.0f } };
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    img_barrier(&c, r.cb, imgs[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdClearColorImage(r.cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1, &rng);
    img_barrier(&c, r.cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (rec_submit(&c, &r, NULL, VK_NULL_HANDLE))
        return 1;
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .swapchainCount = 1, .pSwapchains = &sc,
                            .pImageIndices = &idx };
    VkResult pr = vkQueuePresentKHR(r.q, &pi);
    printf("WSI: headless swapchain of %u %ux%u images, image %u cleared and presented -> %d (%s); "
           "Metal surface %s\n", ni, ext.width, ext.height, idx, pr, vkr(pr), metal ? "offered" : "NOT offered");
    return pr == VK_SUCCESS ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *lib = NULL;
    int loopback = 0, failed = 0, tested = 0, wsi = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--loopback")) loopback = 1;
        else if (!strcmp(argv[i], "--wsi")) wsi = 1;
        else lib = argv[i];
    }
    if (!lib)
#ifdef __APPLE__
        lib = "libvulkan_radeon.dylib";
#else
        lib = "libvulkan_radeon.so";
#endif
    /* RADV exposes its SDMA transfer queue only on request: test it too */
    setenv("RADV_EXPERIMENTAL", "transfer_queue", 0);
    if (loopback)
        setenv("RDNA3_LB_GFX_NOP", "1", 1);
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
    if (wsi)
        return wsi_test();

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
    /* the interop the Metal plugin needs, when RADV offers it */
    IFN(c.inst, vkEnumerateDeviceExtensionProperties);
    const char *want[3] = { "VK_EXT_external_memory_host", "VK_KHR_external_memory_fd",
                            "VK_EXT_mutable_descriptor_type" };
    const char *ext[3];
    uint32_t next = 0, ne = 0;
    int has_host = 0, has_fd = 0, has_mut = 0;
    vkEnumerateDeviceExtensionProperties(c.pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof(*ep));
    if (ep && vkEnumerateDeviceExtensionProperties(c.pd, NULL, &ne, ep) == VK_SUCCESS)
        for (uint32_t i = 0; i < ne; i++)
            for (int w = 0; w < 3; w++)
                if (!strcmp(ep[i].extensionName, want[w])) {
                    ext[next++] = want[w];
                    if (w == 0) has_host = 1; else if (w == 1) has_fd = 1; else has_mut = 1;
                }
    free(ep);
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 hp2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hostp };
    if (has_host)
        vkGetPhysicalDeviceProperties2(c.pd, &hp2);
    printf("  VK_EXT_external_memory_host: %s (alignment %llu), VK_KHR_external_memory_fd: %s\n",
           has_host ? "yes" : "NO", (unsigned long long)hostp.minImportedHostPointerAlignment, has_fd ? "yes" : "NO");
    /* timeline semaphores are a Vulkan 1.2 feature to turn on */
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT fmut = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT,
        .mutableDescriptorType = VK_TRUE };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                             .pNext = has_mut ? &fmut : NULL, .timelineSemaphore = VK_TRUE };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f12, .queueCreateInfoCount = nq,
                               .pQueueCreateInfos = qci, .enabledExtensionCount = next,
                               .ppEnabledExtensionNames = ext };
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
        if (!(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) {
            printf("queue family %u: sparse binding only, no copies to test\n", i);
            continue;
        }
        /* the loopback executes SDMA only: GFX/compute jobs are submitted
         * and fenced (RDNA3_LB_GFX_NOP) but nothing runs, so only the
         * submission path is checked there */
        int verify = !(loopback && !transfer_only);
        /* 64 KiB: RADV's compute-shader fill/copy on GFX/compute queues;
         * 256 B: CP DMA (SDMA either way on a transfer queue) */
        static const VkDeviceSize sizes[2] = { 64 * 1024, 256 };
        for (int s = 0; s < 2; s++) {
            uint32_t pat = 0xc0de0000u | (i << 8) | (uint32_t)s;
            int fr = fill_copy(&c, i, sizes[s], pat, verify);
            printf("%s queue family %u (%s): fill + copy of %llu bytes%s\n", fr ? "FAIL" : "PASS", i, what,
                   (unsigned long long)sizes[s], verify ? "" : " (submitted and fenced, not executed: loopback)");
            failed |= fr != 0;
            tested++;
            if (fr)
                break;   /* a lost device fails every later submit too */
        }
    }
    /* interop on the queue that really executes everywhere: the transfer
     * queue if there is one (the loopback runs SDMA only), else general */
    uint32_t xq = 0;
    for (uint32_t i = 0; i < nq; i++)
        if (!(qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) &&
            (qp[i].queueFlags & VK_QUEUE_TRANSFER_BIT))
            xq = i;
    if (!failed && has_host) {
        int fr = host_ptr_test(&c, xq, hostp.minImportedHostPointerAlignment, 0xb0570000u);
        printf("%s host pointer import (VK_EXT_external_memory_host): GPU fill of 64 KiB of malloc'd memory\n",
               fr ? "FAIL" : "PASS");
        failed |= fr != 0;
        tested++;
    }
    if (!failed && has_fd) {
        int fr = opaque_fd_test(&c, xq, 0xfdfd0000u);
        printf("%s OPAQUE_FD export + import: fill through one, copy out through the other\n", fr ? "FAIL" : "PASS");
        failed |= fr != 0;
        tested++;
    }
    /* graphics, images and timeline semaphores on the general queue (the
     * loopback does not execute GFX: submitted and fenced only there) */
    uint32_t gq = nq;
    for (uint32_t i = 0; i < nq && gq == nq; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            gq = i;
    if (!failed && gq < nq) {
        const char *nx = loopback ? " (submitted and fenced, not executed: loopback)" : "";
        int fr = draw_test(&c, gq, !loopback);
        printf("%s draw: a triangle through a render pass into a 64x64 image, read back%s\n",
               fr ? "FAIL" : "PASS", nx);
        failed |= fr != 0;
        tested++;
        if (!fr) {
            fr = image_copy_test(&c, gq, !loopback);
            printf("%s images: buffer -> image -> image -> buffer of a 64x64 pattern (optimal tiling)%s\n",
                   fr ? "FAIL" : "PASS", nx);
            failed |= fr != 0;
            tested++;
        }
        /* the same on a transfer queue: SDMA tiled copies, which the
         * loopback's software SDMA (linear ops only) cannot run */
        for (uint32_t i = 0; i < nq && !fr && !loopback; i++)
            if (!(qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) &&
                (qp[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
                fr = image_copy_test(&c, i, 1);
                printf("%s images on queue family %u (transfer, SDMA tiled copies)\n", fr ? "FAIL" : "PASS", i);
                failed |= fr != 0;
                tested++;
                break;
            }
        if (!fr) {
            fr = timeline_test(&c, gq);
            printf("%s timeline semaphore: signalled to 7 by a submit, waited for on the host\n",
                   fr ? "FAIL" : "PASS");
            failed |= fr != 0;
            tested++;
        }
    }
    /* last: a RADV without userspace/mesa/patches/0007 asserts here and
     * the assert kills the process, so everything else has run by then */
    if (!failed && has_mut) {
        printf("mutable descriptor layout next: an abort here = RADV without patch 0007\n");
        fflush(stdout);
        int fr = mutable_layout_test(&c);
        printf("%s mutable descriptor layout, bindings out of order (the Metal plugin's layout)\n",
               fr ? "FAIL" : "PASS");
        failed |= fr != 0;
        tested++;
    }
    DFN(c.dev, vkDestroyDevice);
    vkDestroyDevice(c.dev, NULL);
    vkDestroyInstance(c.inst, NULL);
    printf("%s: %d submit(s) tested\n", failed ? "FAILED" : tested ? "ALL PASSED" : "NOTHING TESTED", tested);
    return failed ? 1 : 0;
}
