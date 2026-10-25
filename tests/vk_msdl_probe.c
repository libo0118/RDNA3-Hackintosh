/* Probe, not a unit test (needs a Vulkan device with bufferDeviceAddress + shaderInt64, e.g. the host's RADV; not in `make test`):
 * the multisample texture -> buffer download pass of metal/plugin/nvmtl_msdl.h on a real driver, and the whole of Chess's stencil
 * resolve (card run 2026-10-25f) with the plugin's own passes.
 *
 *   A. a 2/4/8-sample D32_S8 / D16 image is filled with a different stencil and depth value per (tile, sample) by drawing with a
 *      sample mask; the stencil, then the depth, is downloaded into a buffer (Apple's layout: `samples` elements per pixel per row;
 *      a sub-region, an unaligned buffer offset, a padded row, guard bytes around it) and compared byte for byte;
 *   B. colour: random bytes are uploaded into a multisample image with the upload pass (nvmtl_msupload.h, one draw per sample) and
 *      downloaded again: RGBA8, BGRA8, sRGB through the UNORM twin, R8, RG8, R8_UINT, R16_UINT, R32_UINT; every sample must match;
 *   C. Chess's chain at its size, 971x680 4-sample: stencil -> buffer (row 3884) -> 4-sample R8_UINT texture -> resolve ->
 *      1-sample R8_UINT -> buffer (row 971); the result must equal sample 0 of the stencil everywhere.
 *   cc -I metal/plugin -o /tmp/msdl_probe tests/vk_msdl_probe.c -lvulkan && /tmp/msdl_probe
 * SPDX-License-Identifier: MIT */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "nvmtl_msdl.h"
#include "nvmtl_msupload.h"
#include "nvmtl_msdl_spv.h"
#include "nvmtl_msupload_spv.h"
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

static VkDevice dev; static VkPhysicalDevice pd; static VkPhysicalDeviceMemoryProperties mp; static VkQueue queue; static VkCommandPool cpool;
static uint32_t mtype(uint32_t bits, VkMemoryPropertyFlags f) { for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i; exit(3); }
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *map; uint64_t addr; size_t size; } Buf;
static Buf mkbuf(size_t sz)
{
    Buf r; r.size = sz;
    VkBufferCreateInfo bc = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE };
    CK(vkCreateBuffer(dev, &bc, 0, &r.b)); VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, r.b, &mr);
    VkMemoryAllocateFlagsInfo fi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi, mr.size, mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CK(vkAllocateMemory(dev, &ma, 0, &r.m)); CK(vkBindBufferMemory(dev, r.b, r.m, 0)); CK(vkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, (void **)&r.map));
    VkBufferDeviceAddressInfo ai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, r.b }; r.addr = vkGetBufferDeviceAddress(dev, &ai);
    return r;
}
static void freebuf(Buf *b) { vkDestroyBuffer(dev, b->b, 0); vkFreeMemory(dev, b->m, 0); }
static VkImage mkimg(VkFormat f, uint32_t samples, uint32_t w, uint32_t h, VkImageUsageFlags u, VkDeviceMemory *m)
{
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_TYPE_2D, f, { w, h, 1 }, 1, 1, (VkSampleCountFlagBits)samples, VK_IMAGE_TILING_OPTIMAL, u, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage im; VkResult r = vkCreateImage(dev, &ic, 0, &im); if (r) { printf("vkCreateImage fmt %d samples %u -> %d\n", f, samples, r); exit(2); }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im, &mr);
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) }; CK(vkAllocateMemory(dev, &ma, 0, m)); CK(vkBindImageMemory(dev, im, *m, 0)); return im;
}
static VkCommandBuffer begin(void)
{
    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 }; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &ca, &cb));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; CK(vkBeginCommandBuffer(cb, &bi)); return cb;
}
static double submit_wait(VkCommandBuffer cb)
{
    CK(vkEndCommandBuffer(cb)); VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    CK(vkQueueSubmit(queue, 1, &si, 0)); CK(vkQueueWaitIdle(queue));
    clock_gettime(CLOCK_MONOTONIC, &t1); vkFreeCommandBuffers(dev, cpool, 1, &cb);
    return (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
}
static void mem(VkCommandBuffer cb) { VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, 0, 0, 0); }
static void lay(VkCommandBuffer cb, VkImage im, VkImageAspectFlags asp, VkImageLayout o, VkImageLayout n)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, o, n, ~0u, ~0u, im, { asp, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}

/* ---- the plugin's download pass, as nvmtl_vk_cmd_copy_msimage_to_buffer builds it ---- */
struct msd_pc { uint32_t base[2], row, used, w, h; int32_t ox, oy; uint32_t samples, mode, bpp, stride, nwords; };
static VkDescriptorSetLayout dsl; static VkPipelineLayout dpl; static VkPipeline dpipe[2]; static VkDescriptorPool dpool;
static void dl_init(void)
{
    VkDescriptorSetLayoutBinding bnd = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0 };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &bnd }; CK(vkCreateDescriptorSetLayout(dev, &li, 0, &dsl));
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct msd_pc) };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 1, &pr }; CK(vkCreatePipelineLayout(dev, &pl, 0, &dpl));
    for (int u = 0; u < 2; u++) {
        VkShaderModuleCreateInfo sm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, u ? sizeof nvmtl_msdl_uint_spv : sizeof nvmtl_msdl_float_spv, u ? nvmtl_msdl_uint_spv : nvmtl_msdl_float_spv };
        VkShaderModule m; CK(vkCreateShaderModule(dev, &sm, 0, &m));
        VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", 0 }, dpl, 0, -1 };
        CK(vkCreateComputePipelines(dev, 0, 1, &ci, 0, &dpipe[u]));
    }
    VkDescriptorPoolSize sz = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256 };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 256, 1, &sz }; CK(vkCreateDescriptorPool(dev, &pi, 0, &dpool));
}
/* records the download of a region of `img` (already in SHADER_READ_ONLY_OPTIMAL) into buf; returns 0, or -1 when the plan refuses */
static int download(VkCommandBuffer cb, VkImage img, VkFormat fmt, uint32_t samples, uint32_t IW, uint32_t IH, uint32_t aspect_opt,
                    uint32_t x, uint32_t y, uint32_t w, uint32_t h, Buf *buf, size_t off, uint64_t row, nvmtl_msdl_plan *plan, unsigned *nwords_out)
{
    const char *why = 0;
    if (nvmtl_msdl_plan_make(fmt, samples, 4, 1, 1, 0, 0, aspect_opt, IW, IH, x, y, w, h, row, off, buf->size, buf->size, plan, &why)) { printf("  plan refused: %s\n", why); return -1; }
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, img, VK_IMAGE_VIEW_TYPE_2D, (VkFormat)plan->view_fmt, { 0 }, { plan->aspect, 0, 1, 0, 1 } };
    VkImageView view; CK(vkCreateImageView(dev, &vc, 0, &view));
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dpool, 1, &dsl }; VkDescriptorSet set; CK(vkAllocateDescriptorSets(dev, &ai, &set));
    VkDescriptorImageInfo ii = { 0, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii, 0, 0 }; vkUpdateDescriptorSets(dev, 1, &wr, 0, 0);
    const uint64_t base = buf->addr + off; const uint32_t nwords = nvmtl_msbuf_words((uint32_t)(base & 3u), plan->span);
    uint32_t gx, gy, stride; nvmtl_msbuf_dispatch(nwords, &gx, &gy, &stride);
    mem(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dpipe[plan->uint_in]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dpl, 0, 1, &set, 0, 0);
    struct msd_pc pc = { { (uint32_t)base, (uint32_t)(base >> 32) }, (uint32_t)plan->row, (uint32_t)plan->used, w, h, (int32_t)x, (int32_t)y, samples, plan->mode, plan->bpp, stride, nwords };
    vkCmdPushConstants(cb, dpl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, gx, gy, 1);
    mem(cb);
    if (nwords_out) *nwords_out = nwords;
    return 0;
}

/* ---- the plugin's upload pass, as nvmtl_vk_cmd_copy_buffer_to_msimage builds it ---- */
struct msu_pc { uint32_t base[2]; uint32_t row; int32_t ox, oy; uint32_t w, h, mode, samples, sidx, inter; };
static VkPipelineLayout upl;
static void upload(VkCommandBuffer cb, VkImage img, VkFormat fmt, uint32_t samples, uint32_t IW, uint32_t IH, Buf *buf, size_t off, uint64_t row,
                   uint32_t x, uint32_t y, uint32_t w, uint32_t h, nvmtl_msu_plan *plan)
{
    const char *why = 0;
    if (nvmtl_msu_plan_make(fmt, samples, 4, 1, 1, 0, 0, 0, IW, IH, x, y, w, h, row, off, buf->size, plan, &why)) { printf("  upload plan refused: %s\n", why); exit(4); }
    if (!upl) { VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct msu_pc) };
        VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 0, 0, 1, &pr }; CK(vkCreatePipelineLayout(dev, &li, 0, &upl)); }
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, img, VK_IMAGE_VIEW_TYPE_2D, (VkFormat)plan->view_fmt, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view; CK(vkCreateImageView(dev, &vc, 0, &view));
    VkAttachmentDescription ad = { 0, (VkFormat)plan->view_fmt, (VkSampleCountFlagBits)samples, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sd = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 1, &ar };
    VkSubpassDependency dp[2] = { { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
                                  { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 } };
    VkRenderPassCreateInfo rc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 1, &ad, 1, &sd, 2, dp }; VkRenderPass rp; CK(vkCreateRenderPass(dev, &rc, 0, &rp));
    VkShaderModuleCreateInfo s1 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof nvmtl_msu_vert_spv, nvmtl_msu_vert_spv }, s2 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, plan->uint_out ? sizeof nvmtl_msu_frag_u_spv : sizeof nvmtl_msu_frag_spv, plan->uint_out ? nvmtl_msu_frag_u_spv : nvmtl_msu_frag_spv };
    VkShaderModule vm, fm; CK(vkCreateShaderModule(dev, &s1, 0, &vm)); CK(vkCreateShaderModule(dev, &s2, 0, &fm));
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main" }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0 };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, 0, 0, 0, 0, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f };
    VkPipelineMultisampleStateCreateInfo ms_ = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, (VkSampleCountFlagBits)samples };
    VkPipelineColorBlendAttachmentState cba = { 0, 0, 0, 0, 0, 0, 0, 0xf };
    VkPipelineColorBlendStateCreateInfo cb_ = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &cba };
    VkDynamicState dy[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR }; VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dy };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0, 0, 2, st, &vi, &ia, 0, &vp, &rs, &ms_, 0, &cb_, &ds, upl, rp, 0 }; VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, 0, 1, &gp, 0, &pipe));
    VkFramebufferCreateInfo fc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, rp, 1, &view, IW, IH, 1 }; VkFramebuffer fb; CK(vkCreateFramebuffer(dev, &fc, 0, &fb));
    const uint64_t base = buf->addr + off;
    struct msu_pc p = { { (uint32_t)base, (uint32_t)(base >> 32) }, (uint32_t)plan->row, (int32_t)x, (int32_t)y, w, h, plan->mode, samples, 0, plan->inter };
    mem(cb);
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rp, fb, { { 0, 0 }, { IW, IH } }, 0, 0 };
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport v = { 0, 0, (float)IW, (float)IH, 0, 1 }; VkRect2D sc = { { (int32_t)x, (int32_t)y }, { w, h } }; vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc);
    for (uint32_t sm = 0; sm < (plan->inter ? samples : 1u); sm++) { p.sidx = sm; vkCmdPushConstants(cb, upl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p); vkCmdDraw(cb, 3, 1, 0, 0); }
    vkCmdEndRenderPass(cb);
    mem(cb);
}

static int total_bad;
/* compares the whole buffer: guard bytes (0xAA) everywhere but the region, `val(ctx, x, y, s)` little-endian in the region */
typedef uint32_t (*valfn)(void *ctx, uint32_t x, uint32_t y, uint32_t s);
static int check(const char *what, const Buf *buf, size_t off, uint64_t row, uint32_t w, uint32_t h, uint32_t samples, uint32_t bpp, valfn val, void *ctx)
{
    uint8_t *exp = malloc(buf->size); memset(exp, 0xAA, buf->size);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) for (uint32_t s = 0; s < samples; s++) {
        const uint32_t v = val(ctx, x, y, s); const uint64_t o = off + nvmtl_msbuf_elem_offset(row, bpp, samples, x, y, s);
        for (uint32_t k = 0; k < bpp; k++) exp[o + k] = (uint8_t)(v >> (8 * k));
    }
    int bad = 0; uint64_t first = 0;
    for (size_t i = 0; i < buf->size; i++) if (buf->map[i] != exp[i]) { if (!bad) first = i; bad++; }
    if (bad) printf("  %s: %d byte(s) differ, first at %llu: got %02x want %02x\n", what, bad, (unsigned long long)first, buf->map[first], exp[first]);
    else printf("  %s: ok (%u x %u x %u samples x %u B)\n", what, w, h, samples, bpp);
    free(exp); total_bad += bad; return bad;
}

/* ---- A. stencil / depth pattern ---- */
#define TILE 10u
static uint32_t tile_of(uint32_t x, uint32_t y, uint32_t IW) { return (y / TILE) * ((IW + TILE - 1) / TILE) + x / TILE; }
static uint32_t st_ref(uint32_t tile, uint32_t s) { return (1u + tile * 7u + s * 37u) & 0xffu; }
static float dz(uint32_t tile, uint32_t s) { return (float)((tile * 5u + s * 11u) % 64u) / 64.0f; }
struct fillctx { uint32_t IW, kind; };    /* kind 0 stencil, 1 depth32, 2 depth16 */
static uint32_t fill_val(void *c, uint32_t x, uint32_t y, uint32_t s)
{
    struct fillctx *f = c; const uint32_t t = tile_of(x, y, f->IW);
    if (f->kind == 0) return st_ref(t, s);
    float z = dz(t, s); uint32_t b; memcpy(&b, &z, 4);
    return f->kind == 1 ? b : (uint32_t)(z * 65535.0f + 0.5f) & 0xffffu;
}
static VkImage filled_ds(VkFormat fmt, uint32_t samples, uint32_t IW, uint32_t IH, VkDeviceMemory *mem_out, VkImageAspectFlags *asp_out)
{
    const int hasstencil = fmt == VK_FORMAT_D32_SFLOAT_S8_UINT || fmt == VK_FORMAT_D16_UNORM_S8_UINT;
    const VkImageAspectFlags asp = VK_IMAGE_ASPECT_DEPTH_BIT | (hasstencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0); *asp_out = asp;
    VkImage im = mkimg(fmt, samples, IW, IH, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, mem_out);
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, im, VK_IMAGE_VIEW_TYPE_2D, fmt, { 0 }, { asp, 0, 1, 0, 1 } }; VkImageView view; CK(vkCreateImageView(dev, &vc, 0, &view));
    VkAttachmentDescription ad = { 0, fmt, (VkSampleCountFlagBits)samples, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sd = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 0, 0, 0, &ar };
    VkRenderPassCreateInfo rc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 1, &ad, 1, &sd }; VkRenderPass rp; CK(vkCreateRenderPass(dev, &rc, 0, &rp));
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &li, 0, &pl));
    VkShaderModuleCreateInfo s1 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof nvmtl_msu_vert_spv, nvmtl_msu_vert_spv }; VkShaderModule vm; CK(vkCreateShaderModule(dev, &s1, 0, &vm));
    VkPipelineShaderStageCreateInfo st = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main" };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0 };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, 0, 0, 0, 0, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f };
    VkStencilOpState so = { VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 0 };
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, 0, 0, VK_TRUE, VK_TRUE, VK_COMPARE_OP_ALWAYS, 0, hasstencil, so, so, 0, 1 };
    VkDynamicState dy[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE }; VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 3, dy };
    VkPipeline pipe[8];
    for (uint32_t s = 0; s < samples; s++) {   /* one pipeline per sample: its static sample mask */
        VkSampleMask mask = 1u << s;
        VkPipelineMultisampleStateCreateInfo ms_ = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, (VkSampleCountFlagBits)samples, 0, 0, &mask };
        VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0, 0, 1, &st, &vi, &ia, 0, &vp, &rs, &ms_, &dss, 0, &ds, pl, rp, 0 };
        CK(vkCreateGraphicsPipelines(dev, 0, 1, &gp, 0, &pipe[s]));
    }
    VkFramebufferCreateInfo fc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, rp, 1, &view, IW, IH, 1 }; VkFramebuffer fb; CK(vkCreateFramebuffer(dev, &fc, 0, &fb));
    VkCommandBuffer cb = begin();
    VkClearValue cv = { .depthStencil = { 0.5f, 0 } };
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rp, fb, { { 0, 0 }, { IW, IH } }, 1, &cv };
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    const uint32_t tx = (IW + TILE - 1) / TILE, ty = (IH + TILE - 1) / TILE;
    for (uint32_t t = 0; t < tx * ty; t++) for (uint32_t s = 0; s < samples; s++) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[s]);
        const uint32_t x0 = (t % tx) * TILE, y0 = (t / tx) * TILE; VkRect2D sc = { { (int32_t)x0, (int32_t)y0 }, { TILE, TILE } };
        const float z = dz(t, s);
        VkViewport v = { 0, 0, (float)IW, (float)IH, z, z };
        vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, st_ref(t, s));
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cb);
    submit_wait(cb);
    return im;
}

static void fillbuf(Buf *b, uint8_t v) { memset(b->map, v, b->size); }
/* A: download the stencil and the depth of a filled image */
static void test_ds(VkFormat fmt, uint32_t samples, uint32_t IW, uint32_t IH)
{
    VkDeviceMemory m; VkImageAspectFlags asp; VkImage im = filled_ds(fmt, samples, IW, IH, &m, &asp);
    const int hasstencil = (asp & VK_IMAGE_ASPECT_STENCIL_BIT) != 0, d16 = fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D16_UNORM_S8_UINT;
    printf("A. format %d, %u samples, %ux%u\n", fmt, samples, IW, IH);
    struct { uint32_t x, y, w, h; size_t off; uint64_t padrow; } rg[3] = { { 0, 0, IW, IH, 0, 0 }, { 13, 7, IW - 29, IH - 21, 3, 9 }, { 1, 1, 33, 17, 5, 100 } };
    for (int pass = 0; pass < 2; pass++) {   /* 0 stencil, 1 depth */
        if (pass == 0 && !hasstencil) continue;
        const uint32_t bpp = pass == 0 ? 1 : d16 ? 2 : 4; struct fillctx fc = { IW, pass == 0 ? 0 : d16 ? 2 : 1 };
        for (int r = 0; r < 3; r++) {
            const uint32_t w = rg[r].w, h = rg[r].h; const uint64_t row = nvmtl_msbuf_used(w, samples, bpp) + rg[r].padrow;
            Buf buf = mkbuf(rg[r].off + nvmtl_msbuf_span(row, h, w, samples, bpp) + 64); fillbuf(&buf, 0xAA);
            VkCommandBuffer cb = begin(); nvmtl_msdl_plan plan; unsigned nw = 0;
            if (download(cb, im, fmt, samples, IW, IH, pass == 0 ? 4 : 2, rg[r].x, rg[r].y, w, h, &buf, rg[r].off, row, &plan, &nw)) { total_bad++; vkEndCommandBuffer(cb); continue; }
            const double ms = submit_wait(cb);
            /* the expected value of region element (x, y) is the one of image pixel (x + x0, y + y0) */
            struct fillctx c2 = fc; struct { struct fillctx f; uint32_t x0, y0; } cc = { c2, rg[r].x, rg[r].y }; (void)cc;
            uint8_t *exp = malloc(buf.size); memset(exp, 0xAA, buf.size);
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) for (uint32_t sm = 0; sm < samples; sm++) {
                const uint32_t v = fill_val(&fc, x + rg[r].x, y + rg[r].y, sm); const uint64_t o = rg[r].off + nvmtl_msbuf_elem_offset(row, bpp, samples, x, y, sm);
                for (uint32_t k = 0; k < bpp; k++) exp[o + k] = (uint8_t)(v >> (8 * k));
            }
            int bad = 0; size_t first = 0;
            for (size_t i = 0; i < buf.size; i++) if (buf.map[i] != exp[i]) { if (!bad) first = i; bad++; }
            if (bad) printf("  %s region %d (off %zu row %llu): %d byte(s) differ, first at %zu: got %02x want %02x\n", pass == 0 ? "stencil" : "depth", r, rg[r].off, (unsigned long long)row, bad, first, buf.map[first], exp[first]);
            else printf("  %s region %d (%ux%u at %u,%u off %zu row %llu): ok, %u words, %.2f ms\n", pass == 0 ? "stencil" : "depth", r, w, h, rg[r].x, rg[r].y, rg[r].off, (unsigned long long)row, nw, ms);
            total_bad += bad; free(exp); freebuf(&buf);
        }
    }
}
/* B: colour roundtrip through the upload pass */
struct rtctx { const Buf *src; size_t off; uint64_t row; uint32_t samples, bpp; };
static uint32_t rt_val(void *c, uint32_t x, uint32_t y, uint32_t s)
{
    struct rtctx *r = c; uint32_t v = 0; const uint64_t o = r->off + nvmtl_msbuf_elem_offset(r->row, r->bpp, r->samples, x, y, s);
    for (uint32_t k = 0; k < r->bpp; k++) v |= (uint32_t)r->src->map[o + k] << (8 * k);
    return v;
}
static void test_color(VkFormat fmt, const char *name, uint32_t samples, size_t soff, size_t doff)
{
    const uint32_t IW = 100, IH = 80, RX = 10, RY = 5, RW = 61, RH = 47;
    VkDeviceMemory m; VkImage im = mkimg(fmt, samples, IW, IH, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &m);
    nvmtl_msu_plan up; const char *why = 0;
    if (nvmtl_msu_plan_make(fmt, samples, 4, 1, 1, 0, 0, 0, IW, IH, RX, RY, RW, RH, 0, 0, 1u << 30, &up, &why)) { printf("B. %s: upload plan refused (%s)\n", name, why); total_bad++; return; }
    if (!up.inter) { printf("B. %s: the plan chose the flat layout for an interleaved row\n", name); total_bad++; return; }
    const uint32_t bpp = up.bpp; const uint64_t row = nvmtl_msbuf_used(RW, samples, bpp) + 3u * samples * bpp;   /* padded, still a whole number of pixels' samples: the plan keeps it interleaved */
    Buf src = mkbuf(soff + nvmtl_msbuf_span(row, RH, RW, samples, bpp) + 64);
    srand(7 + samples); for (size_t i = 0; i < src.size; i++) src.map[i] = (uint8_t)rand();
    Buf dst = mkbuf(doff + nvmtl_msbuf_span(row, RH, RW, samples, bpp) + 64); fillbuf(&dst, 0xAA);
    VkCommandBuffer cb = begin();
    lay(cb, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    upload(cb, im, fmt, samples, IW, IH, &src, soff, row, RX, RY, RW, RH, &up);
    lay(cb, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    nvmtl_msdl_plan dp;
    if (download(cb, im, fmt, samples, IW, IH, 0, RX, RY, RW, RH, &dst, doff, row, &dp, 0)) { total_bad++; vkEndCommandBuffer(cb); return; }
    submit_wait(cb);
    struct rtctx rc = { &src, soff, row, samples, bpp };
    char what[160]; snprintf(what, sizeof what, "B. %s %u samples (upload mode %u inter %u -> download mode %u), src off %zu dst off %zu", name, samples, up.mode, up.inter, dp.mode, soff, doff);
    check(what, &dst, doff, row, RW, RH, samples, bpp, rt_val, &rc);
    freebuf(&src); freebuf(&dst);
}
/* C: Chess's chain at its size */
static void test_chess(uint32_t IW, uint32_t IH)
{
    const uint32_t S = 4; VkDeviceMemory m; VkImageAspectFlags asp; VkImage ds = filled_ds(VK_FORMAT_D32_SFLOAT_S8_UINT, S, IW, IH, &m, &asp);
    printf("C. Chess chain %ux%u, %u samples: stencil -> buffer (row %u) -> 4-sample R8_UINT -> resolve -> buffer (row %u)\n", IW, IH, S, IW * S, IW);
    Buf b1 = mkbuf(4u << 20), b2 = mkbuf(1u << 20); fillbuf(&b1, 0xAA); fillbuf(&b2, 0xAA);
    VkDeviceMemory m2, m3;
    VkImage ms = mkimg(VK_FORMAT_R8_UINT, S, IW, IH, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &m2);
    VkImage one = mkimg(VK_FORMAT_R8_UINT, 1, IW, IH, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &m3);
    VkCommandBuffer cb = begin(); nvmtl_msdl_plan dp; unsigned nw = 0; nvmtl_msu_plan up;
    if (download(cb, ds, VK_FORMAT_D32_SFLOAT_S8_UINT, S, IW, IH, 4, 0, 0, IW, IH, &b1, 0, IW * S, &dp, &nw)) { total_bad++; return; }
    lay(cb, ms, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    upload(cb, ms, VK_FORMAT_R8_UINT, S, IW, IH, &b1, 0, IW * S, 0, 0, IW, IH, &up);
    lay(cb, one, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageResolve rr = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { IW, IH, 1 } };
    vkCmdResolveImage(cb, ms, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, one, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rr);
    lay(cb, one, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bic = { 0, IW, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { IW, IH, 1 } };
    vkCmdCopyImageToBuffer(cb, one, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b2.b, 1, &bic);
    const double ms_ = submit_wait(cb);
    int bad = 0; size_t first = 0;
    for (uint32_t y = 0; y < IH; y++) for (uint32_t x = 0; x < IW; x++) {
        const uint8_t want = (uint8_t)st_ref(tile_of(x, y, IW), 0), got = b2.map[(size_t)y * IW + x];   /* sample 0, the integer resolve's pick */
        if (got != want) { if (!bad) first = (size_t)y * IW + x; bad++; }
    }
    if (bad) printf("  chain: %d pixel(s) differ from sample 0 of the stencil, first at %zu: got %02x\n", bad, first, b2.map[first]);
    else printf("  chain: ok - all %u pixels of the resolved stencil equal sample 0 (download %u words, whole chain %.2f ms)\n", IW * IH, nw, ms_);
    total_bad += bad; freebuf(&b1); freebuf(&b2);
}
int main(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, 0, 0, 0, 0, VK_API_VERSION_1_3 }; VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai }; VkInstance inst; CK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd); vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); printf("device: %s\n", pp.deviceName);
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, 0 }; f12.bufferDeviceAddress = 1;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 }; f2.features.shaderInt64 = 1;
    float pr = 1; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &pr };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci }; CK(vkCreateDevice(pd, &dci, 0, &dev));
    vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT }; CK(vkCreateCommandPool(dev, &pci, 0, &cpool));
    dl_init();
    /* A */
    test_ds(VK_FORMAT_D32_SFLOAT_S8_UINT, 4, 100, 80);
    test_ds(VK_FORMAT_D32_SFLOAT_S8_UINT, 2, 100, 80);
    test_ds(VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 100, 80);
    test_ds(VK_FORMAT_D16_UNORM, 4, 100, 80);
    /* B */
    const struct { VkFormat f; const char *n; } cf[] = { { VK_FORMAT_R8G8B8A8_UNORM, "RGBA8" }, { VK_FORMAT_B8G8R8A8_UNORM, "BGRA8" }, { VK_FORMAT_B8G8R8A8_SRGB, "BGRA8 sRGB" },
        { VK_FORMAT_R8G8B8A8_SRGB, "RGBA8 sRGB" }, { VK_FORMAT_R8_UNORM, "R8" }, { VK_FORMAT_R8G8_UNORM, "RG8" }, { VK_FORMAT_R8_UINT, "R8_UINT" },
        { VK_FORMAT_R8G8_UINT, "RG8_UINT" }, { VK_FORMAT_R8G8B8A8_UINT, "RGBA8_UINT" }, { VK_FORMAT_R16_UINT, "R16_UINT" }, { VK_FORMAT_R32_UINT, "R32_UINT" } };
    for (unsigned i = 0; i < sizeof cf / sizeof *cf; i++) test_color(cf[i].f, cf[i].n, 4, 0, 0);
    test_color(VK_FORMAT_B8G8R8A8_SRGB, "BGRA8 sRGB", 8, 3, 5);
    test_color(VK_FORMAT_R8_UINT, "R8_UINT", 2, 1, 2);
    test_color(VK_FORMAT_R8G8B8A8_UNORM, "RGBA8", 8, 7, 1);
    /* C */
    test_chess(971, 680);
    printf("TOTAL bad %d\n", total_bad);
    return total_bad != 0;
}
