/* Probe, not a unit test (needs a Vulkan device with bufferDeviceAddress + shaderInt64, e.g. the host's RADV; not in `make test`):
 * the buffer -> multisampled colour upload pass of metal/plugin/nvmtl_msupload.h on a real driver. Uploads a random region
 * into 2/4/8-sample images (sRGB through a UNORM view, R8, RG8, an unaligned buffer offset), resolves, reads back and compares:
 * every texel of the region must equal the buffer's bytes, everything else must stay 0. R8_UINT with Apple's layout (a row holds
 * `samples` elements per pixel, one draw per sample): the integer resolve takes sample 0, so the texel must equal element
 * x * samples + 0. (Per-sample content of all samples is checked by tests/vk_msdl_probe.c, which downloads them again.)
 *   cc -I metal/plugin -o /tmp/msu_probe tests/vk_msupload_probe.c -lvulkan && /tmp/msu_probe
 * SPDX-License-Identifier: MIT */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nvmtl_msupload_spv.h"
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)
static VkDevice dev; static VkPhysicalDevice pd; static VkPhysicalDeviceMemoryProperties mp;
static uint32_t mtype(uint32_t bits, VkMemoryPropertyFlags f) { for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i; exit(3); }
static void mkbuf(VkDeviceSize sz, VkBufferUsageFlags u, VkMemoryPropertyFlags f, VkBuffer *b, VkDeviceMemory *m, void **map)
{
    VkBufferCreateInfo bc = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, sz, u, VK_SHARING_MODE_EXCLUSIVE }; CK(vkCreateBuffer(dev, &bc, 0, b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, *b, &mr);
    VkMemoryAllocateFlagsInfo fi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi, mr.size, mtype(mr.memoryTypeBits, f) }; CK(vkAllocateMemory(dev, &ma, 0, m)); CK(vkBindBufferMemory(dev, *b, *m, 0));
    if (map) CK(vkMapMemory(dev, *m, 0, VK_WHOLE_SIZE, 0, map));
}
static VkImage mkimg(VkFormat f, VkSampleCountFlagBits sc, uint32_t w, uint32_t h, VkImageUsageFlags u, VkDeviceMemory *m)
{
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_TYPE_2D, f, { w, h, 1 }, 1, 1, sc, VK_IMAGE_TILING_OPTIMAL, u, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage im; CK(vkCreateImage(dev, &ic, 0, &im)); VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im, &mr);
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) }; CK(vkAllocateMemory(dev, &ma, 0, m)); CK(vkBindImageMemory(dev, im, *m, 0)); return im;
}
static void bar(VkCommandBuffer cb, VkImage im, VkImageLayout o, VkImageLayout n)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, o, n, ~0u, ~0u, im, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}
static void mem(VkCommandBuffer cb) { VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, 0, 0, 0); }
struct pc { uint32_t base[2]; uint32_t row; int32_t ox, oy; uint32_t w, h, mode, samples, sidx, inter; };
static int run(VkFormat imgfmt, VkFormat viewfmt, uint32_t samples, uint32_t mode, uint32_t bpp, uint32_t offs, int uint_out, uint32_t inter)
{
    const uint32_t IW = 100, IH = 80, RW = 61, RH = 47, OX = 10, OY = 5, ROW = 800;
    VkBuffer sb; VkDeviceMemory sm; uint8_t *sp; mkbuf(ROW * RH + 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &sb, &sm, (void **)&sp);
    srand(7); for (uint32_t i = 0; i < ROW * RH + 64; i++) sp[i] = (uint8_t)rand();
    VkBuffer ob; VkDeviceMemory om; uint8_t *op; mkbuf(IW * IH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ob, &om, (void **)&op);
    VkDeviceMemory m1, m2;
    VkImage ms = mkimg(imgfmt, (VkSampleCountFlagBits)samples, IW, IH, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &m1);
    VkImage ss = mkimg(imgfmt, 1, IW, IH, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &m2);
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, ms, VK_IMAGE_VIEW_TYPE_2D, viewfmt, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }; VkImageView view; CK(vkCreateImageView(dev, &vc, 0, &view));
    VkAttachmentDescription ad = { 0, viewfmt, (VkSampleCountFlagBits)samples, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sd = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 1, &ar };
    VkSubpassDependency dp[2] = { { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
                                  { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 } };
    VkRenderPassCreateInfo rc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 1, &ad, 1, &sd, 2, dp }; VkRenderPass rp; CK(vkCreateRenderPass(dev, &rc, 0, &rp));
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct pc) };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 0, 0, 1, &pr }; VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &li, 0, &pl));
    VkShaderModuleCreateInfo s1 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof nvmtl_msu_vert_spv, nvmtl_msu_vert_spv }, s2 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, uint_out ? sizeof nvmtl_msu_frag_u_spv : sizeof nvmtl_msu_frag_spv, uint_out ? nvmtl_msu_frag_u_spv : nvmtl_msu_frag_spv };
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
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0, 0, 2, st, &vi, &ia, 0, &vp, &rs, &ms_, 0, &cb_, &ds, pl, rp, 0 }; VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, 0, 1, &gp, 0, &pipe));
    VkFramebufferCreateInfo fc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, rp, 1, &view, IW, IH, 1 }; VkFramebuffer fb; CK(vkCreateFramebuffer(dev, &fc, 0, &fb));
    VkCommandPoolCreateInfo pc_ = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; VkCommandPool pool; CK(vkCreateCommandPool(dev, &pc_, 0, &pool));
    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 }; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &ca, &cb));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; CK(vkBeginCommandBuffer(cb, &bi));
    bar(cb, ms, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue z = { { 0, 0, 0, 0 } }; VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }; vkCmdClearColorImage(cb, ms, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &rg);
    bar(cb, ms, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    mem(cb);
    VkBufferDeviceAddressInfo ai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, sb }; VkDeviceAddress addr = vkGetBufferDeviceAddress(dev, &ai) + offs;
    struct pc p = { { (uint32_t)addr, (uint32_t)(addr >> 32) }, ROW, OX, OY, RW, RH, mode, samples, 0, inter };
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rp, fb, { { 0, 0 }, { IW, IH } }, 0, 0 };
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport v = { 0, 0, (float)IW, (float)IH, 0, 1 }; VkRect2D sc = { { OX, OY }, { RW, RH } }; vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc);
    for (uint32_t sm = 0; sm < (inter ? samples : 1u); sm++) { p.sidx = sm; vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p); vkCmdDraw(cb, 3, 1, 0, 0); }
    vkCmdEndRenderPass(cb);
    mem(cb);
    bar(cb, ss, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageResolve rr = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { IW, IH, 1 } };
    vkCmdResolveImage(cb, ms, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ss, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rr);
    bar(cb, ss, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bic = { 0, IW, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { IW, IH, 1 } };
    vkCmdCopyImageToBuffer(cb, ss, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ob, 1, &bic);
    CK(vkEndCommandBuffer(cb));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q); VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb }; CK(vkQueueSubmit(q, 1, &si, 0)); CK(vkQueueWaitIdle(q));
    int bad = 0, inside = 0;
    for (uint32_t y = 0; y < IH; y++) for (uint32_t x = 0; x < IW; x++) {
        int in = x >= OX && x < OX + RW && y >= OY && y < OY + RH; uint8_t exp[4] = { 0, 0, 0, 0 };
        const uint32_t nb = uint_out ? 1 : (mode == 2 ? 1 : mode == 3 ? 2 : 4);
        if (in) { const uint8_t *s = sp + offs + (y - OY) * ROW + (inter ? (uint64_t)(x - OX) * samples * bpp : (uint64_t)(x - OX) * bpp);   /* sample 0 of the pixel */
            if (nb == 1) { exp[0] = s[0]; } else if (nb == 2) { exp[0] = s[0]; exp[1] = s[1]; } else memcpy(exp, s, 4); inside++; }
        const uint8_t *g = op + (y * IW + x) * nb;
        if (in) { if (memcmp(g, exp, nb)) { if (bad < 4) printf("  (%u,%u) got %02x%02x%02x%02x want %02x%02x%02x%02x\n", x, y, g[0], g[1], g[2], g[3], exp[0], exp[1], exp[2], exp[3]); bad++; } }
        else if ((g[0] | (nb > 1 ? g[1] : 0) | (nb > 2 ? g[2] : 0) | (nb > 2 ? g[3] : 0))) { if (bad < 4) printf("  outside (%u,%u) = %02x%02x%02x%02x\n", x, y, g[0], g[1], g[2], g[3]); bad++; }
    }
    printf("fmt %d view %d samples %u mode %u offs %u inter %u: %d bad of %d inside\n", imgfmt, viewfmt, samples, mode, offs, inter, bad, inside);
    return bad;
}
int main(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, 0, 0, 0, 0, VK_API_VERSION_1_3 }; VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai }; VkInstance inst; CK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd); vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, 0 }; f12.bufferDeviceAddress = 1;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 }; f2.features.shaderInt64 = 1;
    float pr = 1; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &pr };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci }; CK(vkCreateDevice(pd, &dci, 0, &dev));
    int bad = 0;
    /* the single-sample layout: every sample of a pixel gets the element at x (float modes: the resolve of identical samples is exact) */
    bad += run(VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, 4, 1, 4, 0, 0, 0);
    bad += run(VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, 4, 1, 4, 3, 0, 0);   /* unaligned buffer offset */
    bad += run(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 4, 1, 4, 0, 0, 0);
    bad += run(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 8, 1, 4, 0, 0, 0);
    bad += run(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 2, 1, 4, 0, 0, 0);
    bad += run(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 4, 0, 4, 0, 0, 0);
    bad += run(VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, 4, 0, 4, 0, 0, 0);
    bad += run(VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 4, 2, 1, 1, 0, 0);
    bad += run(VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, 4, 3, 2, 2, 0, 0);
    /* the integer module (Chess: the 4-sample R8_UINT texture of the stencil resolve), both layouts */
    bad += run(VK_FORMAT_R8_UINT, VK_FORMAT_R8_UINT, 4, 0, 1, 0, 1, 0);
    bad += run(VK_FORMAT_R8_UINT, VK_FORMAT_R8_UINT, 4, 0, 1, 0, 1, 1);   /* sample-interleaved: Apple's layout, sample 0 survives the resolve */
    bad += run(VK_FORMAT_R8_UINT, VK_FORMAT_R8_UINT, 8, 0, 1, 3, 1, 1);
    bad += run(VK_FORMAT_R8_UINT, VK_FORMAT_R8_UINT, 2, 0, 1, 1, 1, 1);
    printf("TOTAL bad %d\n", bad); return bad != 0;
}
