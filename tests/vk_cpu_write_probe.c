/* Probe, not a unit test (needs a Vulkan device, e.g. the host's RADV; not in `make test`): a CPU write AFTER the submit and BEFORE the GPU gets to
 * a buffer -> buffer copy. This is the shape of the plugin's managed upload (metal/plugin/NVMTLObjects.m nvmtl_managed_flush: the copy of the
 * CPU shadow into the VRAM copy rides the next submit and reads the shadow when the GPU runs it) and, with NVMTL_MANAGED_SNAPSHOT=1, of its
 * snapshot (the range is copied into a private buffer at the flush, the GPU copy reads that).
 *   - mode "shadow": the copy's source is the CPU buffer; the CPU rewrites it right after vkQueueSubmit while the GPU is still busy with 40
 *     fills of 1 GiB ahead of the copy: the destination holds the REWRITTEN bytes (a mix of two states when the rewrite is partway, here
 *     the whole range is rewritten, so all of it);
 *   - mode "snapshot": the source is a private copy made before the submit: the destination holds the bytes as of the flush.
 * Both are plain Vulkan semantics (host writes are visible to a device read when the device runs it); the probe makes the window concrete and
 * measures it. Exit 0 when "shadow" shows the rewrite and "snapshot" shows the original.
 *   cc -o /tmp/cpu_write_probe tests/vk_cpu_write_probe.c -lvulkan && /tmp/cpu_write_probe
 * SPDX-License-Identifier: MIT */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)
static VkDevice dev; static VkPhysicalDeviceMemoryProperties mp; static VkQueue queue; static VkCommandPool pool;
static uint32_t mtype(uint32_t bits, VkMemoryPropertyFlags f) { for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i; printf("no memory type\n"); exit(3); }
static void mkbuf(VkDeviceSize size, VkBufferUsageFlags u, VkMemoryPropertyFlags f, VkBuffer *b, VkDeviceMemory *m, void **map)
{
    VkBufferCreateInfo bc = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, u, VK_SHARING_MODE_EXCLUSIVE }; CK(vkCreateBuffer(dev, &bc, 0, b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, *b, &mr);
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mtype(mr.memoryTypeBits, f) };
    CK(vkAllocateMemory(dev, &ma, 0, m)); CK(vkBindBufferMemory(dev, *b, *m, 0)); if (map) CK(vkMapMemory(dev, *m, 0, VK_WHOLE_SIZE, 0, map));
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
enum { N = 64 * 1024 };
static int run(int snapshot, double *window_ms)
{
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkBuffer shadow, snap, dst, big, rb; VkDeviceMemory ms, mn, md, mb, mr_; uint8_t *ps, *pn, *pr;
    mkbuf(N, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hv, &shadow, &ms, (void **)&ps);
    mkbuf(N, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hv, &snap, &mn, (void **)&pn);
    mkbuf(N, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &dst, &md, 0);
    mkbuf(1ull << 30, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &big, &mb, 0);
    mkbuf(N, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hv, &rb, &mr_, (void **)&pr);
    memset(ps, 0xAA, N);                                     /* the state at didModifyRange / the flush */
    if (snapshot) memcpy(pn, ps, N);                         /* nvmtl_managed_flush with NVMTL_MANAGED_SNAPSHOT=1 */
    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 }; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &ca, &cb));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }; CK(vkBeginCommandBuffer(cb, &bi));
    VkMemoryBarrier mbar = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT };
    for (int i = 0; i < 40; i++) {                           /* the queue ahead of the copy: other work in flight */
        vkCmdFillBuffer(cb, big, 0, 1ull << 30, 0x11111111u + (uint32_t)i);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mbar, 0, 0, 0, 0);
    }
    VkBufferCopy bc = { 0, 0, N }; vkCmdCopyBuffer(cb, snapshot ? snap : shadow, dst, 1, &bc);
    VkMemoryBarrier tb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &tb, 0, 0, 0, 0);
    vkCmdCopyBuffer(cb, dst, rb, 1, &bc);
    VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, 0, 0, 0);
    CK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence fence; CK(vkCreateFence(dev, &fi, 0, &fence));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb }; const double t0 = now(); CK(vkQueueSubmit(queue, 1, &si, fence));
    memset(ps, 0xBB, N);                                     /* the app rewrites the range right after commit / "scheduled" */
    const double t1 = now();
    CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 20ull * 1000 * 1000 * 1000)); *window_ms = now() - t0;
    unsigned a = 0, b = 0; for (int i = 0; i < N; i++) { a += pr[i] == 0xAA; b += pr[i] == 0xBB; }
    printf("%-9s the CPU rewrote the source %.3f ms after the submit, the GPU finished %.1f ms after it: destination 0x%02x %u of %d bytes (flush state), 0x%02x %u (rewrite)\n",
           snapshot ? "snapshot" : "shadow", t1 - t0, *window_ms, 0xAA, a, N, 0xBB, b);
    return snapshot ? (a == N) : (b == N);
}
int main(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "cpu_write", 1, "x", 1, VK_API_VERSION_1_3 }; VkInstanceCreateInfo ic = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai };
    VkInstance in; CK(vkCreateInstance(&ic, 0, &in)); uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(in, &n, &pd); vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    float pr = 1; VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &pr }; VkDeviceCreateInfo dc = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &q };
    CK(vkCreateDevice(pd, &dc, 0, &dev)); vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 }; CK(vkCreateCommandPool(dev, &cpi, 0, &pool));
    double w; int ok = run(0, &w); ok &= run(1, &w);
    printf(ok ? "ok: a copy reads its CPU source when the GPU runs it; a snapshot made at the flush keeps the state of the flush\n"
              : "UNEXPECTED: the window was not wide enough on this machine (the GPU was faster than the CPU's rewrite) or the semantics differ\n");
    return ok ? 0 : 1;
}
