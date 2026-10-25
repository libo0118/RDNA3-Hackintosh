/* Probe, not a unit test (needs a Vulkan device, e.g. the host's RADV; not in `make test`): does RADV's compression metadata (DCC, and
 * the 4x multisample compression) stay consistent through the image-state sequences metal/plugin/nvmtl_vk.c really uses? Card run
 * 2026-10-25e: tile-shaped blocks of wrong colour in Chess (GL over the plugin), broken glyphs in Terminal/Maps. Blocky garbage is the
 * classic sign of metadata that does not match the data, so every way the plugin lies to the driver about an image is exercised on
 * images big enough for DCC (the printed memory size is smaller with RADV_DEBUG=nodcc: that is DCC's metadata), in memory the probe first fills with 0xCD
 * (so stale metadata is garbage, as on a card whose VRAM held other images):
 *   - a LOAD render pass begins with initialLayout TRANSFER_SRC_OPTIMAL whatever layout the image rests in (the plugin's
 *     nvmtl_make_render_pass declares it; the image may rest in SHADER_READ_ONLY (the Chess log's "G11 a Load pass began on an image
 *     resting in layout 5"), or have never been written (UNDEFINED));
 *   - replaceRegion-style uploads: barrier from the tracked layout to TRANSFER_DST (srcStage TOP_OF_PIPE, srcAccess 0), a buffer ->
 *     image copy of a sub-rectangle, barrier to SHADER_READ_ONLY (nvmtl_vk_image_write_region_level_layer);
 *   - compute kernels writing the image through a storage view in GENERAL layout, its own rgba8 view or an R32_UINT reinterpretation (the GL
 *     renderer's / ImageIO's storage textures, `+storage` in the Chess log);
 *   - sampling through the image's own view, and through an R32_UINT view of a 4-byte colour image (Metal pixel-format views);
 *   - an sRGB image rendered through a UNORM view ("encoder: gl3 colour attachment in its VIEW's format 44 over a format-50 image"),
 *     with a fast clear (LOAD_OP_CLEAR to an arbitrary colour), also 4x multisample with a pResolveAttachments resolve into a
 *     second mutable sRGB image (Chess's GL passes: "par1 4-sample pass resolves colour 0x0 of 0x1").
 * Every image is created like the plugin's (MUTABLE_FORMAT, no VkImageFormatListCreateInfo, COLOR_ATTACHMENT | INPUT_ATTACHMENT |
 * TRANSFER_SRC | TRANSFER_DST | SAMPLED). A CPU model of the contents is kept; after the random op sequence the image is read back and
 * every pixel the model defines must match. Env: SEED=n (default 1), OPS=n (default 80), BATCH=n (operations per command buffer, default 1; 16 is the plugin's shape), HONEST=1 (barrier to the declared layout first:
 * the control), NOPOLLUTE=1, NOC2C=1 (no barrier between two compute dispatches: the negative control, it must FAIL with 16x16-tile-shaped wrong regions).
 *   cc -o /tmp/meta_probe tests/vk_meta_probe.c -lvulkan -lm && /tmp/meta_probe
 *   RADV_DEBUG=nodcc /tmp/meta_probe        (the metadata off: must agree)
 *   LIE=1 /tmp/meta_probe                   (render-pass samples WITHOUT a layout transition, like the plugin's render encoders; LIE=2 also leaves uploads in TRANSFER_DST)
 * Shaders (glslc --target-env=vulkan1.1 -O; the SPIR-V is in vk_meta_probe_spv.h):
 *   q.vert   : fullscreen triangle from gl_VertexIndex (a rectangle is a scissor)
 *   pat.frag : o = hash(gl_FragCoord.xy, pc.seed) as 4 bytes (r, g, b, 255) / 255   (exact in a UNORM 8-bit target)
 *   smp.frag : o = texelFetch(sampler2D, ivec2(gl_FragCoord.xy), 0)
 *   smpu.frag: o = texelFetch(usampler2D, ...)  into an R32_UINT target
 *   st.comp / stu.comp: imageStore of the same hash into an rgba8 / r32ui storage view of a rectangle
 * SPDX-License-Identifier: MIT */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "vk_meta_probe_spv.h"
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

static VkDevice dev; static VkPhysicalDevice pd; static VkPhysicalDeviceMemoryProperties mp; static VkQueue queue; static uint32_t qfam;
static VkCommandPool pool; static VkSampler g_samp; static VkDescriptorSetLayout g_dsl; static VkPipelineLayout g_pl; static VkDescriptorPool g_dp;
static VkShaderModule g_vs, g_pat, g_smp, g_smpu, g_st, g_stu; static int g_honest, g_pollute = 1;
static VkDescriptorSetLayout g_dsl_st; static VkPipelineLayout g_pl_st; static VkPipeline g_cp_st[2]; static VkDescriptorPool g_dp_st;
static uint32_t mtype(uint32_t bits, VkMemoryPropertyFlags f) { for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i; printf("no memory type\n"); exit(3); }
static VkShaderModule mkmod(const uint32_t *w, size_t bytes) { VkShaderModuleCreateInfo c = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, bytes, w }; VkShaderModule m; CK(vkCreateShaderModule(dev, &c, 0, &m)); return m; }
static uint64_t g_rng;
static uint32_t rnd(void) { g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(g_rng >> 33); }

typedef struct { VkImage img; VkDeviceMemory mem; VkDeviceSize size; VkFormat fmt; uint32_t w, h, bpp, samples; VkImageLayout lay; VkImageUsageFlags usage; uint32_t *model; uint8_t *known; } subj;

static VkCommandBuffer begincb(void)
{ VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 }; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &ca, &cb));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }; CK(vkBeginCommandBuffer(cb, &bi)); return cb; }
static void endcb(VkCommandBuffer cb)
{ CK(vkEndCommandBuffer(cb)); VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb }; CK(vkQueueSubmit(queue, 1, &si, 0)); CK(vkQueueWaitIdle(queue)); vkFreeCommandBuffers(dev, pool, 1, &cb); }

static void mkbuf(VkDeviceSize size, VkBufferUsageFlags u, VkBuffer *b, VkDeviceMemory *m, void **map)
{
    VkBufferCreateInfo bc = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, u, VK_SHARING_MODE_EXCLUSIVE }; CK(vkCreateBuffer(dev, &bc, 0, b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, *b, &mr);
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CK(vkAllocateMemory(dev, &ma, 0, m)); CK(vkBindBufferMemory(dev, *b, *m, 0)); CK(vkMapMemory(dev, *m, 0, VK_WHOLE_SIZE, 0, map));
}

/* an image like nvmtl_vk_image_create_typed's: mutable, no format list; memory filled with 0xCD first when polluting */
static subj mksubj(VkFormat fmt, uint32_t w, uint32_t h, uint32_t samples, VkImageUsageFlags usage)
{
    subj s; memset(&s, 0, sizeof s); s.fmt = fmt; s.w = w; s.h = h; s.samples = samples; s.usage = usage; s.lay = VK_IMAGE_LAYOUT_UNDEFINED;
    s.bpp = fmt == VK_FORMAT_R8_UNORM ? 1 : 4;
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_TYPE_2D, fmt, { w, h, 1 }, 1, 1, (VkSampleCountFlagBits)samples,
        VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED };
    CK(vkCreateImage(dev, &ic, 0, &s.img)); VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, s.img, &mr); s.size = mr.size;
    VkBufferCreateInfo bc = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, mr.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE }; VkBuffer pb; CK(vkCreateBuffer(dev, &bc, 0, &pb));
    VkMemoryRequirements bm; vkGetBufferMemoryRequirements(dev, pb, &bm);
    uint32_t bits = mr.memoryTypeBits & bm.memoryTypeBits;
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size > bm.size ? mr.size : bm.size, mtype(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CK(vkAllocateMemory(dev, &ma, 0, &s.mem)); CK(vkBindImageMemory(dev, s.img, s.mem, 0)); CK(vkBindBufferMemory(dev, pb, s.mem, 0));
    if (g_pollute) { VkCommandBuffer cb = begincb(); vkCmdFillBuffer(cb, pb, 0, mr.size & ~3ull, 0xCDCDCDCDu); endcb(cb); }
    vkDestroyBuffer(dev, pb, 0);
    s.model = calloc((size_t)w * h, 4); s.known = calloc((size_t)w * h, 1);
    return s;
}
static VkImageView mkview(VkImage img, VkFormat f)
{ VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, img, VK_IMAGE_VIEW_TYPE_2D, f, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }; VkImageView v; CK(vkCreateImageView(dev, &vc, 0, &v)); return v; }

static void bar(VkCommandBuffer cb, VkImage im, VkImageLayout o, VkImageLayout n, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    if (o == n) return;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, sa, da, o, n, ~0u, ~0u, im, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, 0, 0, 0, 1, &b);
}
/* the plugin's img_to(): srcAccess MEMORY_RW, ALL_COMMANDS -> COMPUTE_SHADER stage (nvmtl_vk_cmd_prepare_sampled_image) */
static int g_trace, g_noc2c, g_lie, g_lie_n[16];   /* NOC2C=1: no barrier between two compute dispatches (the negative control: what a missing ordering looks like) */
static void to_sampled(VkCommandBuffer cb, subj *s)
{ bar(cb, s->img, s->lay, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  s->lay = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
static void to_src(VkCommandBuffer cb, subj *s)
{ bar(cb, s->img, s->lay, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  s->lay = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; }

/* nvmtl_make_render_pass: colour [+ resolve]; LOAD declares initialLayout TRANSFER_SRC, a clear pass UNDEFINED; both end in TRANSFER_SRC */
static VkRenderPass mkrp(VkFormat f, uint32_t samples, int load, int resolve)
{
    VkAttachmentDescription ad[2] = {
        { 0, f, (VkSampleCountFlagBits)samples, load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
          load ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
        { 0, f, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL } };
    VkAttachmentReference cr = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, rr = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sd = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 1, &cr, resolve ? &rr : 0 };
    VkSubpassDependency dp[2] = { { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
                                  { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 } };
    VkRenderPassCreateInfo rc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, resolve ? 2u : 1u, ad, 1, &sd, 2, dp }; VkRenderPass rp; CK(vkCreateRenderPass(dev, &rc, 0, &rp)); return rp;
}
static VkPipeline mkpipe(VkRenderPass rp, VkShaderModule fs, uint32_t samples)
{
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, g_vs, "main" }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0 };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, 0, 0, 0, 0, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, (VkSampleCountFlagBits)samples };
    VkPipelineColorBlendAttachmentState cba = { 0, 0, 0, 0, 0, 0, 0, 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &cba };
    VkDynamicState dy[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR }; VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dy };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0, 0, 2, st, &vi, &ia, 0, &vp, &rs, &ms, 0, &cb, &ds, g_pl, rp, 0 };
    VkPipeline p; CK(vkCreateGraphicsPipelines(dev, 0, 1, &gp, 0, &p)); return p;
}
static void setup(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "meta_probe", 1, "x", 1, VK_API_VERSION_1_3 }; VkInstanceCreateInfo ic = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai };
    VkInstance in; CK(vkCreateInstance(&ic, 0, &in)); uint32_t n = 1; CK(vkEnumeratePhysicalDevices(in, &n, &pd) & ~VK_INCOMPLETE); vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); printf("device: %s\n", pp.deviceName);
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, 0); VkQueueFamilyProperties qf[16]; if (qn > 16) qn = 16; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    for (uint32_t i = 0; i < qn; i++) if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { qfam = i; break; }
    float pr = 1; VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, qfam, 1, &pr }; VkDeviceCreateInfo dc = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &q };
    CK(vkCreateDevice(pd, &dc, 0, &dev)); vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam }; CK(vkCreateCommandPool(dev, &cpi, 0, &pool));
    VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    CK(vkCreateSampler(dev, &sc, 0, &g_samp));
    VkDescriptorSetLayoutBinding lb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0 }; VkDescriptorSetLayoutCreateInfo lc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &lb };
    CK(vkCreateDescriptorSetLayout(dev, &lc, 0, &g_dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8 }; VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &g_dsl, 1, &pcr }; CK(vkCreatePipelineLayout(dev, &pl, 0, &g_pl));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 }; VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 4096, 1, &ps }; CK(vkCreateDescriptorPool(dev, &dpi, 0, &g_dp));
    g_vs = mkmod(meta_q_vert_spv, sizeof meta_q_vert_spv); g_pat = mkmod(meta_pat_frag_spv, sizeof meta_pat_frag_spv);
    g_smp = mkmod(meta_smp_frag_spv, sizeof meta_smp_frag_spv); g_smpu = mkmod(meta_smpu_frag_spv, sizeof meta_smpu_frag_spv);
    g_st = mkmod(meta_st_comp_spv, sizeof meta_st_comp_spv); g_stu = mkmod(meta_stu_comp_spv, sizeof meta_stu_comp_spv);
    VkDescriptorSetLayoutBinding sb = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0 }; VkDescriptorSetLayoutCreateInfo slc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &sb };
    CK(vkCreateDescriptorSetLayout(dev, &slc, 0, &g_dsl_st));
    VkPushConstantRange spr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 20 }; VkPipelineLayoutCreateInfo spl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &g_dsl_st, 1, &spr }; CK(vkCreatePipelineLayout(dev, &spl, 0, &g_pl_st));
    VkShaderModule cms[2] = { g_st, g_stu };
    for (int i = 0; i < 2; i++) { VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, cms[i], "main" }, g_pl_st };
        CK(vkCreateComputePipelines(dev, 0, 1, &cpi, 0, &g_cp_st[i])); }
    VkDescriptorPoolSize sps = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096 }; VkDescriptorPoolCreateInfo sdp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 4096, 1, &sps }; CK(vkCreateDescriptorPool(dev, &sdp, 0, &g_dp_st));
}
static VkDescriptorSet mkset(VkImageView v)
{
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, g_dp, 1, &g_dsl }; VkDescriptorSet s; CK(vkAllocateDescriptorSets(dev, &ai, &s));
    VkDescriptorImageInfo ii = { g_samp, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, s, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii }; vkUpdateDescriptorSets(dev, 1, &w, 0, 0); return s;
}

/* the model: what pattern(x, y, seed) leaves in the image's bytes */
static uint32_t pat_dword(VkFormat f, uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t h = x * 0x9E3779B1u ^ y * 0x85EBCA77u ^ seed * 0xC2B2AE3Du; h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    uint32_t r = h & 255, g = (h >> 8) & 255, b = (h >> 16) & 255, a = 255;
    if (f == VK_FORMAT_R8_UNORM) return r;
    if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB) return b | g << 8 | r << 16 | a << 24;
    return r | g << 8 | b << 16 | a << 24;
}
typedef struct { int bad; uint32_t first[2]; uint32_t got, want; int tiles; } cmp;
static void compare(const char *what, const subj *s, const uint8_t *bytes, VkFormat viewfmt_unused, cmp *out)
{
    (void)viewfmt_unused; memset(out, 0, sizeof *out); static uint8_t *seen; static size_t seen_n;
    const size_t tw = (s->w + 15) / 16, th = (s->h + 15) / 16; if (seen_n < tw * th) { free(seen); seen = malloc(tw * th); seen_n = tw * th; } memset(seen, 0, tw * th);
    for (uint32_t y = 0; y < s->h; y++) for (uint32_t x = 0; x < s->w; x++) {
        if (!s->known[(size_t)y * s->w + x]) continue;
        uint32_t got = s->bpp == 1 ? bytes[(size_t)y * s->w + x] : ((const uint32_t *)bytes)[(size_t)y * s->w + x], want = s->model[(size_t)y * s->w + x];
        if (got != want) { if (!out->bad) { out->first[0] = x; out->first[1] = y; out->got = got; out->want = want; } out->bad++; if (!seen[(y / 16) * tw + x / 16]) { seen[(y / 16) * tw + x / 16] = 1; out->tiles++; } }
    }
    (void)what;
}
/* ---- batching: BATCH=n puts n operations into ONE command buffer (the plugin's real shape: Chess records 825 draws and many transitions in one) ---- */
typedef struct { subj t; uint32_t *mdl; uint8_t *known; VkBuffer b; VkDeviceMemory m; void *p; const char *tag; int opn; int keep; } pend;
static pend g_pend[512]; static int g_npend, g_batch = 1, g_nin; static VkCommandBuffer g_bcb; static long g_bad; static uint8_t *g_last; static size_t g_last_n;
static VkBuffer g_dbuf[1024]; static VkDeviceMemory g_dmem[1024]; static int g_ndbuf; static VkFramebuffer g_dfb[1024]; static int g_ndfb;
static VkCommandBuffer opcb(void) { if (!g_bcb) g_bcb = begincb(); return g_bcb; }
static void defer_buf(VkBuffer b, VkDeviceMemory m) { g_dbuf[g_ndbuf] = b; g_dmem[g_ndbuf++] = m; }
static void defer_fb(VkFramebuffer f) { g_dfb[g_ndfb++] = f; }
static void batch_flush(void)
{
    if (!g_bcb) return;
    endcb(g_bcb); g_bcb = 0; g_nin = 0;
    for (int i = 0; i < g_npend; i++) {
        pend *q = &g_pend[i]; subj v = q->t; v.model = q->mdl; v.known = q->known; cmp c; compare(q->tag, &v, (const uint8_t *)q->p, 0, &c);
        if (c.bad) { printf("  op %d %s: %d bad pixels in %d 16x16 tiles; first (%u,%u) got 0x%08x want 0x%08x\n", q->opn, q->tag, c.bad, c.tiles, c.first[0], c.first[1], c.got, c.want); g_bad += c.bad; }
        if (q->keep) { size_t n = (size_t)v.w * v.h * v.bpp; if (g_last_n < n) { free(g_last); g_last = malloc(n); g_last_n = n; } memcpy(g_last, q->p, n); }
        vkUnmapMemory(dev, q->m); vkDestroyBuffer(dev, q->b, 0); vkFreeMemory(dev, q->m, 0); free(q->mdl); free(q->known);
    }
    g_npend = 0;
    for (int i = 0; i < g_ndbuf; i++) { vkDestroyBuffer(dev, g_dbuf[i], 0); vkFreeMemory(dev, g_dmem[i], 0); }
    g_ndbuf = 0;
    for (int i = 0; i < g_ndfb; i++) vkDestroyFramebuffer(dev, g_dfb[i], 0);
    g_ndfb = 0;
}
static void op_done(void) { if (++g_nin >= g_batch || g_npend > 400 || g_ndbuf > 900 || g_ndfb > 900) batch_flush(); }
/* record the copy of s (in TRANSFER_SRC after to_src) into a host buffer; compare against `mdl`/`known` (snapshots) once the batch is submitted */
static void record_readback(VkCommandBuffer cb, subj *s, uint32_t bpp, const uint32_t *model, const uint8_t *known, const char *tag, int opn, int keep)
{
    pend *q = &g_pend[g_npend++]; memset(q, 0, sizeof *q); q->t = *s; q->t.bpp = bpp; q->tag = tag; q->opn = opn; q->keep = keep;
    mkbuf((VkDeviceSize)s->w * s->h * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &q->b, &q->m, &q->p);
    q->mdl = malloc((size_t)s->w * s->h * 4); q->known = malloc((size_t)s->w * s->h); memcpy(q->mdl, model, (size_t)s->w * s->h * 4); memcpy(q->known, known, (size_t)s->w * s->h);
    to_src(cb, s);
    VkBufferImageCopy bic = { 0, s->w, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { s->w, s->h, 1 } };
    vkCmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, q->b, 1, &bic);
}

/* ---- the operations ------------------------------------------------------------------------------------------------------------ */
static VkRenderPass g_rp_load[3], g_rp_clr[3]; static VkPipeline g_pipe_pat[3], g_pipe_smp[3];   /* [0] BGRA UNORM, [1] RGBA UNORM, [2] R8 */
static int fidx(VkFormat f) { return f == VK_FORMAT_R8_UNORM ? 2 : (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB) ? 0 : 1; }
static void draw_rect(VkCommandBuffer cb, VkPipeline p, uint32_t w, uint32_t h, int x, int y, int rw, int rh, uint32_t seed, VkDescriptorSet set)
{
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkViewport v = { 0, 0, (float)w, (float)h, 0, 1 }; VkRect2D sc = { { x, y }, { (uint32_t)rw, (uint32_t)rh } }; vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc);
    uint32_t pc[2] = { seed, 0 }; vkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, pc);
    if (set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 0, 1, &set, 0, 0);
    vkCmdDraw(cb, 3, 1, 0, 0);
}
static void model_fill(subj *s, int x, int y, int w, int h, uint32_t seed)
{ for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) { s->model[(size_t)j * s->w + i] = pat_dword(s->fmt, i, j, seed); s->known[(size_t)j * s->w + i] = 1; } }
/* a LOAD pass over the image, as the plugin begins one: declared TRANSFER_SRC, whatever the image rests in */
static void op_fill(subj *s, VkImageView view, int x, int y, int w, int h, uint32_t seed)
{
    const int fi = fidx(s->fmt); VkCommandBuffer cb = opcb();
    if (g_honest) bar(cb, s->img, s->lay, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkFramebufferCreateInfo fc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, g_rp_load[fi], 1, &view, s->w, s->h, 1 }; VkFramebuffer fb; CK(vkCreateFramebuffer(dev, &fc, 0, &fb));
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, g_rp_load[fi], fb, { { 0, 0 }, { s->w, s->h } }, 0, 0 };
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE); draw_rect(cb, g_pipe_pat[fi], s->w, s->h, x, y, w, h, seed, 0); vkCmdEndRenderPass(cb);
    defer_fb(fb); s->lay = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; model_fill(s, x, y, w, h, seed); op_done();
}
/* a compute kernel writing the image through a storage view (variant 0: the image's own rgba8 view, 1: an R32_UINT reinterpretation), the image in GENERAL:
 * nvmtl_vk_cmd_prepare_sampled_image(storage = 1) and the GL renderer's / ImageIO's storage textures */
static void op_compute(subj *s, int x, int y, int w, int h, uint32_t seed, int variant)
{
    VkImageView v = mkview(s->img, variant ? VK_FORMAT_R32_UINT : VK_FORMAT_R8G8B8A8_UNORM);
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, g_dp_st, 1, &g_dsl_st }; VkDescriptorSet set; CK(vkAllocateDescriptorSets(dev, &ai, &set));
    VkDescriptorImageInfo ii = { VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii }; vkUpdateDescriptorSets(dev, 1, &wr, 0, 0);
    VkCommandBuffer cb = opcb();
    bar(cb, s->img, s->lay, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    s->lay = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_cp_st[variant]); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_st, 0, 1, &set, 0, 0);
    uint32_t pc[5] = { seed, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h }; vkCmdPushConstants(cb, g_pl_st, VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    { /* nvmtl_vk_cmd_barrier_c2c: the plugin orders one dispatch after the other */
      VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
      if (!g_noc2c) vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, 0, 0, 0); }
    model_fill(s, x, y, w, h, seed); op_done();
}
/* replaceRegion: nvmtl_vk_image_write_region_level_layer */
static void op_upload(subj *s, int x, int y, int w, int h)
{
    VkBuffer b; VkDeviceMemory m; uint8_t *p; mkbuf((VkDeviceSize)w * h * s->bpp, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &b, &m, (void **)&p);
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) { uint32_t v = rnd(); if (s->bpp == 1) p[j * w + i] = (uint8_t)v; else ((uint32_t *)p)[j * w + i] = v | 0xFF000000u; }
    VkCommandBuffer cb = opcb();
    /* the plugin's replaceRegion is a submit of its own (srcStage TOP_OF_PIPE, srcAccess 0 is enough there: everything before it is complete); in a batch it needs the real scope */
    if (g_batch > 1) bar(cb, s->img, s->lay, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    else bar(cb, s->img, s->lay, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy bic = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { x, y, 0 }, { (uint32_t)w, (uint32_t)h, 1 } };
    vkCmdCopyBufferToImage(cb, b, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    if (g_lie == 2 && (rnd() & 1)) s->lay = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;   /* LIE=2: a copy that does not transition back (the image rests in TRANSFER_DST when it is sampled) */
    else {
    bar(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    s->lay = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) { s->model[(size_t)(y + j) * s->w + x + i] = s->bpp == 1 ? p[j * w + i] : ((uint32_t *)p)[j * w + i]; s->known[(size_t)(y + j) * s->w + x + i] = 1; }
    vkUnmapMemory(dev, m); defer_buf(b, m); op_done();
}
/* sample the image into a fresh target (same format; or R32_UINT through a 32-bit view); the whole target is compared with the model of the moment */
static void op_sample(subj *s, VkFormat svf, int as_r32, const char *tag, int opn)
{
    if (!svf) svf = s->fmt;
    subj t = mksubj(as_r32 ? VK_FORMAT_R32_UINT : svf, s->w, s->h, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImageView sv = mkview(s->img, as_r32 ? VK_FORMAT_R32_UINT : svf), tv = mkview(t.img, t.fmt);
    VkRenderPass rp = mkrp(t.fmt, 1, 0, 0); VkPipeline pp = mkpipe(rp, as_r32 ? g_smpu : g_smp, 1);
    VkDescriptorSet set = mkset(sv); VkCommandBuffer cb = opcb();
    if (g_lie) {   /* LIE=1 (N51 ECC round 1): what the plugin's render encoders do - NO layout transition before a sampled bind: the descriptor declares SHADER_READ_ONLY
                    * whatever the image rests in (TRANSFER_SRC after a render pass, GENERAL after a compute write, TRANSFER_DST). Only a memory dependency, so the layout lie is
                    * the one thing that differs from the honest probe. */
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, 0, 0, 0);
        g_lie_n[(unsigned)s->lay < 16 ? (unsigned)s->lay : 15]++;
    } else to_sampled(cb, s);
    VkFramebufferCreateInfo fc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, rp, 1, &tv, t.w, t.h, 1 }; VkFramebuffer fb; CK(vkCreateFramebuffer(dev, &fc, 0, &fb));
    VkClearValue cv; memset(&cv, 0, sizeof cv); VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rp, fb, { { 0, 0 }, { t.w, t.h } }, 1, &cv };
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE); draw_rect(cb, pp, t.w, t.h, 0, 0, t.w, t.h, 0, set); vkCmdEndRenderPass(cb);
    defer_fb(fb); t.lay = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    record_readback(cb, &t, as_r32 ? 4 : s->bpp, s->model, s->known, tag, opn, 0); free(t.model); free(t.known); op_done();
}
static void op_readback(subj *s, const char *tag, int opn, int keep)
{ VkCommandBuffer cb = opcb(); record_readback(cb, s, s->bpp, s->model, s->known, tag, opn, keep); op_done(); }
static int run_seq(const char *name, VkFormat f, uint32_t w, uint32_t h, int nops, int first_partial, int stmode)
{
    subj s = mksubj(f, w, h, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | (stmode ? VK_IMAGE_USAGE_STORAGE_BIT : 0));
    VkImageView view = mkview(s.img, f); int nsamp = 0, nrb = 0; g_bad = 0;
    for (int i = 0; i < nops; i++) {
        int rw = 1 + (int)(rnd() % (rnd() % 4 == 0 ? w / 2 : 96)), rh = 1 + (int)(rnd() % (rnd() % 4 == 0 ? h / 2 : 96)); if (rw > (int)w) rw = w; if (rh > (int)h) rh = h;
        int x = (int)(rnd() % (w - rw + 1)), y = (int)(rnd() % (h - rh + 1)); uint32_t k = rnd() % 100;
        if (i == 0 && first_partial) k = 0;                 /* the first thing that ever happens to the image is a LOAD pass over a rectangle */
        else if (i == 0) k = 50;                            /* ... or an upload */
        if (g_trace) printf("    op %d: k=%u rect %d,%d %dx%d lay=%d\n", i, k, x, y, rw, rh, (int)s.lay);
        if (stmode && k % 4 == 3 && i) op_compute(&s, x, y, rw, rh, rnd(), stmode - 1);
        else if (k < 40) op_fill(&s, view, x, y, rw, rh, rnd());
        else if (k < 70) op_upload(&s, x, y, rw, rh);
        else if (k < 85) { nsamp++; op_sample(&s, 0, 0, "sample (same view)", i); }
        else if (k < 92 && f != VK_FORMAT_R8_UNORM) { nsamp++; op_sample(&s, 0, 1, "sample (R32_UINT view)", i); }
        else { nrb++; op_readback(&s, "readback", i, 0); }
    }
    op_readback(&s, "final readback", nops, 1); batch_flush();
    { long garb = 0, unk = 0; for (uint32_t i = 0; i < w * h; i++) if (!s.known[i]) { unk++; if (s.bpp == 1 ? g_last[i] : ((uint32_t *)g_last)[i]) garb++; }
      if (unk) printf("  (info) %ld of %ld pixels the sequence never defined read back non-zero (stale memory / metadata: expected, undefined by the API)\n", garb, unk); }
    printf("%-46s %ux%u, memory %llu B%s, %d ops in batches of %d (%d samples, %d readbacks%s), %s start: %ld bad\n", name, w, h, (unsigned long long)s.size, g_pollute ? " polluted" : "", nops, g_batch, nsamp, nrb,
           stmode == 1 ? ", compute writes through an rgba8 storage view" : stmode == 2 ? ", compute writes through an R32_UINT storage view" : "",
           first_partial ? "LOAD-pass-on-a-fresh-image" : "upload", g_bad);
    return g_bad ? 1 : 0;
}

/* sRGB image through a UNORM view; fast clear; 4x multisample + resolve (Chess) */
static int run_srgb(int ms, int rounds)
{
    const VkFormat img = VK_FORMAT_B8G8R8A8_SRGB, vf = VK_FORMAT_B8G8R8A8_UNORM; const uint32_t W = 971, H = 680; int bad = 0;
    const VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkRenderPass rp = mkrp(vf, ms ? 4 : 1, 0, ms), rpl = mkrp(vf, ms ? 4 : 1, 1, ms);
    VkPipeline pp = mkpipe(rp, g_pat, ms ? 4 : 1);
    subj r = mksubj(img, W, H, 1, u); subj m; memset(&m, 0, sizeof m); if (ms) m = mksubj(img, W, H, 4, u);
    VkImageView rv = mkview(r.img, vf), mv = ms ? mkview(m.img, vf) : 0; g_bad = 0;
    for (int round = 0; round < rounds; round++) {
        const int load = round > 0 && (rnd() & 1);
        float c[4] = { (rnd() % 256) / 255.f, (rnd() % 256) / 255.f, (rnd() % 256) / 255.f, (rnd() % 256) / 255.f }; if (round % 5 == 0) c[0] = c[1] = c[2] = c[3] = 0.f; if (round % 5 == 1) c[0] = c[1] = c[2] = c[3] = 1.f;
        const int rx = rnd() % (W / 2), ry = rnd() % (H / 2), rw = 1 + rnd() % (W / 2), rh = 1 + rnd() % (H / 2); const uint32_t seed = rnd();
        VkCommandBuffer cb = opcb();
        VkImageView att[2] = { ms ? mv : rv, rv }; VkRenderPass use = load ? rpl : rp;
        VkFramebufferCreateInfo fc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, use, ms ? 2u : 1u, att, W, H, 1 }; VkFramebuffer fb; CK(vkCreateFramebuffer(dev, &fc, 0, &fb));
        VkClearValue cv[2]; memset(cv, 0, sizeof cv); memcpy(cv[0].color.float32, c, 16);
        VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, use, fb, { { 0, 0 }, { W, H } }, 2, cv };
        vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE); draw_rect(cb, pp, W, H, rx, ry, rw, rh, seed, 0); vkCmdEndRenderPass(cb); defer_fb(fb); op_done();
        r.lay = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; if (ms) m.lay = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        /* model (all in the UNORM view's byte domain: the same bytes the sRGB image holds) */
        for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
            uint32_t i = y * W + x; const int in = (int)x >= rx && (int)x < rx + rw && (int)y >= ry && (int)y < ry + rh;
            if (in) { r.model[i] = pat_dword(vf, x, y, seed); r.known[i] = 1; }
            else if (!load) { r.model[i] = (uint32_t)lrintf(c[2] * 255) | (uint32_t)lrintf(c[1] * 255) << 8 | (uint32_t)lrintf(c[0] * 255) << 16 | (uint32_t)lrintf(c[3] * 255) << 24; r.known[i] = 1; }
        }
        op_sample(&r, vf, 0, ms ? "ms resolve -> sample (UNORM view)" : "clear/load -> sample (UNORM view)", round);
        op_sample(&r, vf, 1, ms ? "ms resolve -> sample (R32_UINT view)" : "clear/load -> sample (R32_UINT view)", round);
    }
    batch_flush(); bad = g_bad ? 1 : 0;
    printf("%-46s %ux%u sRGB image through a UNORM view, %d rounds in batches of %d (clear/load, fast clear colours incl. 0 and 1): %ld bad\n", ms ? "4x multisample + resolve" : "1 sample", W, H, rounds, g_batch, g_bad);
    return bad;
}
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_rng = (uint64_t)(getenv("SEED") ? atoi(getenv("SEED")) : 1) * 0x9E3779B97F4A7C15ull + 1; g_honest = getenv("HONEST") != 0; g_noc2c = getenv("NOC2C") != 0; g_trace = getenv("TRACE") != 0; g_lie = getenv("LIE") ? atoi(getenv("LIE")) : 0; g_pollute = !getenv("NOPOLLUTE");
    const int nops = getenv("OPS") ? atoi(getenv("OPS")) : 80; g_batch = getenv("BATCH") ? atoi(getenv("BATCH")) : 1; if (g_batch < 1) g_batch = 1;
    setup();
    const VkFormat fs[3] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM };
    for (int i = 0; i < 3; i++) { g_rp_load[i] = mkrp(fs[i], 1, 1, 0); g_rp_clr[i] = mkrp(fs[i], 1, 0, 0); g_pipe_pat[i] = mkpipe(g_rp_load[i], g_pat, 1); g_pipe_smp[i] = mkpipe(g_rp_load[i], g_smp, 1); }
    if (g_lie) printf("LIE=%d: render-pass samples are recorded WITHOUT a layout transition (the plugin's render encoders)\n", g_lie);
    printf("honest layouts: %s, polluted memory: %s\n", g_honest ? "yes (control)" : "no (the plugin's declared TRANSFER_SRC)", g_pollute ? "yes" : "no");
    int bad = 0;
    bad += run_seq("BGRA8 colour image, plugin-style sequence", VK_FORMAT_B8G8R8A8_UNORM, 1920, 1080, nops, 0, 0);
    bad += run_seq("BGRA8 colour image, plugin-style sequence", VK_FORMAT_B8G8R8A8_UNORM, 1920, 1080, nops, 1, 0);
    bad += run_seq("RGBA8 texture, plugin-style sequence", VK_FORMAT_R8G8B8A8_UNORM, 1024, 1024, nops, 0, 0);
    bad += run_seq("RGBA8 texture, plugin-style sequence", VK_FORMAT_R8G8B8A8_UNORM, 1024, 1024, nops, 1, 1);
    bad += run_seq("RGBA8 texture, plugin-style sequence", VK_FORMAT_R8G8B8A8_UNORM, 1024, 1024, nops, 0, 2);
    bad += run_seq("R8 glyph atlas, plugin-style sequence", VK_FORMAT_R8_UNORM, 1024, 1024, nops, 0, 0);
    bad += run_seq("R8 glyph atlas, plugin-style sequence", VK_FORMAT_R8_UNORM, 1024, 1024, nops, 1, 0);
    bad += run_srgb(0, 40);
    bad += run_srgb(1, 40);
    if (g_lie) printf("samples taken with the image resting in: UNDEFINED %d, GENERAL %d, SHADER_READ_ONLY %d, TRANSFER_SRC %d, TRANSFER_DST %d (declared SHADER_READ_ONLY)\n",
                      g_lie_n[0], g_lie_n[1], g_lie_n[5], g_lie_n[6], g_lie_n[7]);
    printf(bad ? "FAILED: %d bad pixels\n" : "ok: no pixel the model defines is wrong (%d)\n", bad);
    return bad ? 1 : 0;
}
