/* Probe, not a unit test (needs a Vulkan device, e.g. the host's RADV; not in `make test`): Metal framebuffer fetch
 * ([[color(n)]]) on a real driver, in the exact shape metal/plugin/nvmtl_vk.c uses:
 *   - the colour attachment is also subpass input attachment n of the SAME subpass, both references in layout GENERAL;
 *   - a by-region self-dependency (COLOR_ATTACHMENT_OUTPUT/WRITE -> FRAGMENT_SHADER/INPUT_ATTACHMENT_READ) and a
 *     vkCmdPipelineBarrier with the same masks recorded inside the pass before every draw that reads;
 *   - the input attachments live in descriptor set 3 (binding 192 + n, INPUT_ATTACHMENT, fragment stage) of a pipeline layout whose
 *     sets 0..2 are empty layouts; the fragment module is patched with nvmtl_spirv_fbfetch_set() (the plugin's own patch);
 *   - the attachment is 1920x1080 so DCC is on; the pass begins from / ends in TRANSFER_SRC_OPTIMAL like the plugin's.
 * Cases: one fetch draw over a gradient for several formats (incl. sRGB view over an sRGB image, UNORM view over an sRGB image);
 * 40 overlapping fetch draws that add 0.01 each (per-pixel read-after-write ordering, exact in RGBA32F; env NOBARRIER=1 drops
 * the barrier as a negative control and only reports); a restart (pass A without input attachments, pass B with, LOADing).
 *   cc -I metal/plugin -o /tmp/fbf_probe tests/vk_fbfetch_probe.c -lvulkan -lm && /tmp/fbf_probe
 * Shaders (glslc --target-env=vulkan1.1 -O; the SPIR-V is in vk_fbfetch_probe_spv.h):
 *   fbf.vert  : fullscreen triangle from gl_VertexIndex
 *   grad.frag : o = vec4(gl_FragCoord.xy / pc.size, 0.25, 1.0) * pc.mul        (pc = { vec2 size; float mul; float add; })
 *   fetch.frag: layout(input_attachment_index=0, set=0, binding=192) uniform subpassInput u0;
 *               o = subpassLoad(u0) * pc.mul + vec4(pc.add, pc.add, pc.add, 0.0);
 * SPDX-License-Identifier: MIT */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "nvmtl_spirv_fbfetch.h"
#include "vk_fbfetch_probe_spv.h"
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)
#define FBF_SET 3u
#define W 1920u
#define H 1080u
static VkDevice dev; static VkPhysicalDevice pd; static VkPhysicalDeviceMemoryProperties mp; static VkQueue queue; static uint32_t qfam;
static uint32_t mtype(uint32_t bits, VkMemoryPropertyFlags f) { for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i; exit(3); }
static VkShaderModule mkmod(const uint32_t *w, size_t bytes)
{ VkShaderModuleCreateInfo c = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, bytes, w }; VkShaderModule m; CK(vkCreateShaderModule(dev, &c, 0, &m)); return m; }
struct pc { float size[2]; float mul, add; };
static VkPipelineLayout g_pl; static VkDescriptorSetLayout g_empty, g_fbf;
static VkPipeline mkpipe(VkRenderPass rp, VkShaderModule vm, VkShaderModule fm)
{
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main" }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0 };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, 0, 0, 0, 0, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { 0, 0, 0, 0, 0, 0, 0, 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &cba };
    VkDynamicState dy[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR }; VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dy };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0, 0, 2, st, &vi, &ia, 0, &vp, &rs, &ms, 0, &cb, &ds, g_pl, rp, 0 };
    VkPipeline p; CK(vkCreateGraphicsPipelines(dev, 0, 1, &gp, 0, &p)); return p;
}
/* the plugin's nvmtl_make_render_pass(): input = 1 adds pInputAttachments {colour, GENERAL}, colour layout GENERAL and the self-dependency */
static VkRenderPass mkpass(VkFormat f, int input, int load)
{
    VkAttachmentDescription ad = { 0, f, VK_SAMPLE_COUNT_1_BIT, load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, load ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ar = { 0, input ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, ir = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sd = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, input ? 1u : 0u, input ? &ir : 0, 1, &ar };
    VkSubpassDependency dp[3] = { { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
                                  { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
                                  { 0, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT } };
    VkRenderPassCreateInfo rc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 1, &ad, 1, &sd, input ? 3u : 2u, dp }; VkRenderPass rp; CK(vkCreateRenderPass(dev, &rc, 0, &rp)); return rp;
}
static void fbf_barrier(VkCommandBuffer cb)
{
    if (getenv("NOBARRIER")) return;
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, 0, 0, 0);
}
static float h2f(uint16_t h) { int e = (h >> 10) & 31, m = h & 1023; float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? 65504.f : ldexpf((float)(m + 1024), e - 25); return (h >> 15) ? -v : v; }
static float srgb_dec(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkBuffer ob; VkDeviceMemory om; void *op; VkFramebuffer fb[2]; } target;
static target mktarget(VkFormat imgfmt, VkFormat viewfmt, VkRenderPass rp0, VkRenderPass rp1)
{
    target t; memset(&t, 0, sizeof t);
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_TYPE_2D, imgfmt, { W, H, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED };
    CK(vkCreateImage(dev, &ic, 0, &t.img)); VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, t.img, &mr);
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) }; CK(vkAllocateMemory(dev, &ma, 0, &t.mem)); CK(vkBindImageMemory(dev, t.img, t.mem, 0));
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, t.img, VK_IMAGE_VIEW_TYPE_2D, viewfmt, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CK(vkCreateImageView(dev, &vc, 0, &t.view));
    VkBufferCreateInfo bc = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, (VkDeviceSize)W * H * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE }; CK(vkCreateBuffer(dev, &bc, 0, &t.ob));
    vkGetBufferMemoryRequirements(dev, t.ob, &mr); VkMemoryAllocateInfo mb = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CK(vkAllocateMemory(dev, &mb, 0, &t.om)); CK(vkBindBufferMemory(dev, t.ob, t.om, 0)); CK(vkMapMemory(dev, t.om, 0, VK_WHOLE_SIZE, 0, &t.op));
    VkRenderPass rps[2] = { rp0, rp1 };
    for (int i = 0; i < 2; i++) if (rps[i]) { VkFramebufferCreateInfo fc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, rps[i], 1, &t.view, W, H, 1 }; CK(vkCreateFramebuffer(dev, &fc, 0, &t.fb[i])); }
    return t;
}
static void bar(VkCommandBuffer cb, VkImage im, VkImageLayout o, VkImageLayout n)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, o, n, ~0u, ~0u, im, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}
static VkCommandPool pool;
static VkCommandBuffer begincb(void)
{ VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 }; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &ca, &cb));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; CK(vkBeginCommandBuffer(cb, &bi)); return cb; }
static void finish(VkCommandBuffer cb, target *t)
{
    VkBufferImageCopy bic = { 0, W, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->ob, 1, &bic);
    CK(vkEndCommandBuffer(cb)); VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb }; CK(vkQueueSubmit(queue, 1, &si, 0)); CK(vkQueueWaitIdle(queue));
}
/* the pixel at (x,y) as 4 floats in the VIEW's value domain (sRGB decoded when the view is sRGB) */
static void px(const target *t, VkFormat f, uint32_t x, uint32_t y, float o[4])
{
    const uint8_t *b = (const uint8_t *)t->op;
    switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: { const uint8_t *p = b + ((size_t)y * W + x) * 4; o[0] = p[2] / 255.f; o[1] = p[1] / 255.f; o[2] = p[0] / 255.f; o[3] = p[3] / 255.f; break; }
        case VK_FORMAT_R8G8B8A8_UNORM: { const uint8_t *p = b + ((size_t)y * W + x) * 4; for (int i = 0; i < 4; i++) o[i] = p[i] / 255.f; break; }
        case VK_FORMAT_R16G16B16A16_SFLOAT: { const uint16_t *p = (const uint16_t *)(b + ((size_t)y * W + x) * 8); for (int i = 0; i < 4; i++) o[i] = h2f(p[i]); break; }
        default: { const float *p = (const float *)(b + ((size_t)y * W + x) * 16); for (int i = 0; i < 4; i++) o[i] = p[i]; break; }
    }
}
static void draw(VkCommandBuffer cb, VkPipeline p, float mul, float add, int x, int y, int w, int h)
{
    struct pc c = { { (float)W, (float)H }, mul, add };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkViewport v = { 0, 0, (float)W, (float)H, 0, 1 }; VkRect2D sc = { { x, y }, { (uint32_t)w, (uint32_t)h } }; vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof c, &c); vkCmdDraw(cb, 3, 1, 0, 0);
}
/* like the plugin: the descriptor's view is a view of its own (usage INPUT_ATTACHMENT only), not the framebuffer's */
static VkDescriptorSet mkset(VkImage img, VkFormat fmt)
{
    VkImageViewUsageCreateInfo uc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, 0, VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT };
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &uc, 0, img, VK_IMAGE_VIEW_TYPE_2D, fmt, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView v; CK(vkCreateImageView(dev, &vc, 0, &v));
    static VkDescriptorPool dp; if (!dp) { VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 64 }; VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 64, 1, &ps }; CK(vkCreateDescriptorPool(dev, &pi, 0, &dp)); }
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 1, &g_fbf }; VkDescriptorSet s; CK(vkAllocateDescriptorSets(dev, &ai, &s));
    VkDescriptorImageInfo ii = { VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, s, 192, 0, 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &ii }; vkUpdateDescriptorSets(dev, 1, &w, 0, 0); return s;
}
static VkShaderModule g_vm, g_gm, g_fm;
static int check_tol(const char *what, float got, float want, float tol, int *bad, uint32_t x, uint32_t y, int ch)
{ if (fabsf(got - want) > tol) { if (*bad < 4) printf("  %s (%u,%u) ch%d got %.5f want %.5f\n", what, x, y, ch, got, want); (*bad)++; return 1; } return 0; }
/* case 1: gradient (non-fetch pipeline) then one fetch draw: o = in * 0.5 + 0.125 (alpha: in.a * 0.5), everything in ONE pass */
static int run_one(VkFormat imgfmt, VkFormat viewfmt, const char *name)
{
    VkRenderPass rp = mkpass(viewfmt, 1, 0); VkPipeline pg = mkpipe(rp, g_vm, g_gm), pf = mkpipe(rp, g_vm, g_fm);
    target t = mktarget(imgfmt, viewfmt, rp, 0); VkDescriptorSet set = mkset(t.img, viewfmt);
    VkCommandBuffer cb = begincb(); bar(cb, t.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkClearValue cv = { { { 0, 0, 0, 0 } } }; VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rp, t.fb[0], { { 0, 0 }, { W, H } }, 1, &cv };
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    draw(cb, pg, 1.f, 0.f, 0, 0, W, H);
    fbf_barrier(cb); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, FBF_SET, 1, &set, 0, 0);
    draw(cb, pf, 0.5f, 0.125f, 0, 0, W, H);
    vkCmdEndRenderPass(cb); finish(cb, &t);
    int bad = 0; const int srgb = viewfmt == VK_FORMAT_B8G8R8A8_SRGB, f16 = viewfmt == VK_FORMAT_R16G16B16A16_SFLOAT, f32 = viewfmt == VK_FORMAT_R32G32B32A32_SFLOAT;
    const float tol = f32 ? 2e-5f : f16 ? 2e-3f : srgb ? 0.02f : 1.6f / 255.f;
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
        float g[4] = { (x + .5f) / W, (y + .5f) / H, .25f, 1.f }, got[4], want[4]; px(&t, viewfmt, x, y, got);
        for (int c = 0; c < 4; c++) want[c] = g[c] * .5f + (c < 3 ? .125f : 0.f);
        for (int c = 0; c < 4; c++) { float gv = srgb && c < 3 ? srgb_dec(got[c]) : got[c]; check_tol(name, gv, want[c], tol, &bad, x, y, c); }
    }
    printf("%-34s one pass, gradient then fetch: %d bad of %u\n", name, bad, W * H * 4); return bad;
}
/* case 2: RGBA32F, a gradient then 40 overlapping fetch draws, each adding 0.01 in its scissor rect: exact read-after-write ordering */
static int run_order(void)
{
    VkFormat f = VK_FORMAT_R32G32B32A32_SFLOAT; VkRenderPass rp = mkpass(f, 1, 0); VkPipeline pg = mkpipe(rp, g_vm, g_gm), pf = mkpipe(rp, g_vm, g_fm);
    target t = mktarget(f, f, rp, 0); VkDescriptorSet set = mkset(t.img, f);
    VkCommandBuffer cb = begincb(); bar(cb, t.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkClearValue cv = { { { 0, 0, 0, 0 } } }; VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rp, t.fb[0], { { 0, 0 }, { W, H } }, 1, &cv };
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    draw(cb, pg, 1.f, 0.f, 0, 0, W, H);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, FBF_SET, 1, &set, 0, 0);
    enum { N = 40 }; int rx[N], ry[N], rw[N], rh[N]; srand(11);
    for (int i = 0; i < N; i++) { rw[i] = 300 + rand() % 900; rh[i] = 200 + rand() % 600; rx[i] = rand() % (W - rw[i]); ry[i] = rand() % (H - rh[i]);
        fbf_barrier(cb); draw(cb, pf, 1.f, 0.01f, rx[i], ry[i], rw[i], rh[i]); }
    vkCmdEndRenderPass(cb); finish(cb, &t);
    int bad = 0, wrong_px = 0;
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
        int cnt = 0; for (int i = 0; i < N; i++) if ((int)x >= rx[i] && (int)x < rx[i] + rw[i] && (int)y >= ry[i] && (int)y < ry[i] + rh[i]) cnt++;
        float got[4]; px(&t, f, x, y, got); float g[3] = { (x + .5f) / W, (y + .5f) / H, .25f }; int b0 = bad;
        for (int c = 0; c < 3; c++) check_tol("order", got[c], g[c] + 0.01f * cnt, 3e-5f, &bad, x, y, c);
        check_tol("order", got[3], 1.f, 1e-6f, &bad, x, y, 3); if (bad != b0) wrong_px++;
    }
    printf("%-34s %d overlapping fetch draws%s: %d wrong pixels (%d channel errors)\n", "R32G32B32A32_SFLOAT", N, getenv("NOBARRIER") ? " WITHOUT the barrier (control)" : "", wrong_px, bad);
    return getenv("NOBARRIER") ? 0 : bad;
}
/* case 3: the plugin's restart: pass A has no input attachments (gradient), pass B (LOAD, input attachments, begun with the SAME framebuffer) fetches */
static int run_restart(VkFormat f, const char *name)
{
    VkRenderPass rpA = mkpass(f, 0, 0), rpB = mkpass(f, 1, 1); VkPipeline pg = mkpipe(rpA, g_vm, g_gm), pf = mkpipe(rpB, g_vm, g_fm);
    target t = mktarget(f, f, rpA, rpB); VkDescriptorSet set = mkset(t.img, f);
    VkCommandBuffer cb = begincb(); bar(cb, t.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkClearValue cv = { { { 0, 0, 0, 0 } } }; VkRenderPassBeginInfo ra = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rpA, t.fb[0], { { 0, 0 }, { W, H } }, 1, &cv };
    vkCmdBeginRenderPass(cb, &ra, VK_SUBPASS_CONTENTS_INLINE); draw(cb, pg, 1.f, 0.f, 0, 0, W, H); vkCmdEndRenderPass(cb);
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rpB, t.fb[1], { { 0, 0 }, { W, H } }, 0, 0 };
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    fbf_barrier(cb); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, FBF_SET, 1, &set, 0, 0);
    draw(cb, pf, 0.5f, 0.125f, 0, 0, W, H); vkCmdEndRenderPass(cb); finish(cb, &t);
    int bad = 0; const float tol = f == VK_FORMAT_R32G32B32A32_SFLOAT ? 2e-5f : 1.6f / 255.f;
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
        float g[4] = { (x + .5f) / W, (y + .5f) / H, .25f, 1.f }, got[4]; px(&t, f, x, y, got);
        for (int c = 0; c < 4; c++) check_tol(name, got[c], g[c] * .5f + (c < 3 ? .125f : 0.f), tol, &bad, x, y, c);
    }
    printf("%-34s restart (pass A without input attachments, pass B with): %d bad\n", name, bad); return bad;
}
/* case 4 (write-after-read): fetch draws and ordinary draws interleaved over overlapping rectangles in ONE pass, the barrier recorded only
 * before the fetch draws (what the plugin does). An ordinary draw that follows a fetch draw overwrites pixels the fetch draw just read: the
 * result must be the CPU replay of the sequence. RGBA32F so that a missing ordering shows exactly. */
static int run_mixed(void)
{
    VkFormat f = VK_FORMAT_R32G32B32A32_SFLOAT; VkRenderPass rp = mkpass(f, 1, 0); VkPipeline pg = mkpipe(rp, g_vm, g_gm), pf = mkpipe(rp, g_vm, g_fm);
    target t = mktarget(f, f, rp, 0); VkDescriptorSet set = mkset(t.img, f);
    VkCommandBuffer cb = begincb(); bar(cb, t.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkClearValue cv = { { { 0, 0, 0, 0 } } }; VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rp, t.fb[0], { { 0, 0 }, { W, H } }, 1, &cv };
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, FBF_SET, 1, &set, 0, 0);
    enum { N = 60 }; int kind[N], rx[N], ry[N], rw[N], rh[N]; srand(23);
    for (int i = 0; i < N; i++) {
        kind[i] = i == 0 ? 0 : rand() % 2;   /* 0: ordinary draw (gradient * 0.5 replaces), 1: fetch draw (+0.01) */
        rw[i] = 300 + rand() % 900; rh[i] = 200 + rand() % 600; rx[i] = rand() % (W - rw[i]); ry[i] = rand() % (H - rh[i]);
        if (kind[i]) { fbf_barrier(cb); draw(cb, pf, 1.f, 0.01f, rx[i], ry[i], rw[i], rh[i]); }
        else draw(cb, pg, 0.5f, 0.f, rx[i], ry[i], rw[i], rh[i]);
    }
    vkCmdEndRenderPass(cb); finish(cb, &t);
    int bad = 0, wrong_px = 0;
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
        float v[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < N; i++) if ((int)x >= rx[i] && (int)x < rx[i] + rw[i] && (int)y >= ry[i] && (int)y < ry[i] + rh[i]) {
            if (kind[i]) v[0] += 0.01f, v[1] += 0.01f, v[2] += 0.01f;
            else { v[0] = (x + .5f) / W * .5f; v[1] = (y + .5f) / H * .5f; v[2] = .125f; v[3] = .5f; }
        }
        float got[4]; px(&t, f, x, y, got); int b0 = bad;
        for (int c = 0; c < 4; c++) check_tol("mixed", got[c], v[c], 5e-5f, &bad, x, y, c);
        if (bad != b0) wrong_px++;
    }
    printf("%-34s %d interleaved fetch / ordinary draws (write after read): %d wrong pixels (%d channel errors)\n", "R32G32B32A32_SFLOAT", N, wrong_px, bad);
    return bad;
}
int main(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, 0, 0, 0, 0, VK_API_VERSION_1_3 }; VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai }; VkInstance inst; CK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd); vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props); printf("device: %s\n", props.deviceName);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, 0); VkQueueFamilyProperties *qp = (VkQueueFamilyProperties *)calloc(nq, sizeof *qp); vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq && !(qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT); qfam++) ;
    float pr = 1; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, qfam, 1, &pr };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qci }; CK(vkCreateDevice(pd, &dci, 0, &dev)); vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, qfam }; CK(vkCreateCommandPool(dev, &pci, 0, &pool));
    VkDescriptorSetLayoutCreateInfo el = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; CK(vkCreateDescriptorSetLayout(dev, &el, 0, &g_empty));
    VkDescriptorSetLayoutBinding lb = { 192, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo fl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &lb }; CK(vkCreateDescriptorSetLayout(dev, &fl, 0, &g_fbf));
    VkDescriptorSetLayout sl[4] = { g_empty, g_empty, g_empty, g_fbf }; VkPushConstantRange pr_ = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct pc) };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 4, sl, 1, &pr_ }; CK(vkCreatePipelineLayout(dev, &li, 0, &g_pl));
    /* the fetch module goes through the plugin's own patches: the variable moves from set 0 to set 3 */
    size_t nw = sizeof fbf_fetch_spv / 4; uint32_t *fw = (uint32_t *)malloc(sizeof fbf_fetch_spv); memcpy(fw, fbf_fetch_spv, sizeof fbf_fetch_spv);
    nvmtl_fbfetch scan; size_t nw2 = nw; int rc = nvmtl_spirv_fbfetch(fw, &nw2, nw, 0, &scan);
    int moved = nvmtl_spirv_fbfetch_set(fw, nw, FBF_SET);
    printf("scan rc %d decl 0x%x read 0x%x reads %u, set patch moved %d decoration(s)\n", rc, scan.decl_mask, scan.read_mask, scan.reads, moved);
    if (rc || scan.decl_mask != 1 || scan.read_mask != 1 || moved != 1) { printf("FAIL: the plugin's scan / set patch did not see the fetch variable\n"); return 1; }
    g_vm = mkmod(fbf_vert_spv, sizeof fbf_vert_spv); g_gm = mkmod(fbf_grad_spv, sizeof fbf_grad_spv); g_fm = mkmod(fw, nw * 4);
    int bad = 0;
    bad += run_one(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM");
    bad += run_one(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM");
    bad += run_one(VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, "B8G8R8A8_SRGB (sRGB view)");
    bad += run_one(VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_SRGB image, UNORM view");
    bad += run_one(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT");
    bad += run_one(VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT");
    bad += run_order();
    bad += run_mixed();
    bad += run_restart(VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM");
    bad += run_restart(VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT");
    printf("TOTAL bad %d\n", bad); return bad != 0;
}
