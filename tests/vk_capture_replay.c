/* Probe, not a unit test (needs a Vulkan device: the host's RADV; not in `make test`). DRIVER-PLAN N56: replays one draw the
 * Metal plugin captured (NVMTL_CAPTURE_DRAW, tools/capture_decode.py --out DIR unpacks it) of Core Animation's
 * buffer_image_blit_vert/frag_lpf glyph pipeline on this machine's GPU, into a B8G8R8A8 target laid out like the card's:
 * linear, explicit DRM modifier layout, row pitch from the capture (2560 for Terminal's 608x353 window), the capture's viewport
 * and scissor, blend One / OneMinusSrcAlpha over black. It reads the target back and compares every pixel inside the scissor
 * with a CPU evaluation of the same shader on the same bytes (the cpu_draw snapshots): the A8 coverage times the colour.
 *   mode "desc"  : the translator's SPIR-V as captured, buffers as storage-buffer descriptors (the fragment module moved to set 1)
 *   mode "table" : the SPIR-V lowered as the plugin lowers it (librdna3mtl_translate's nvmtl_lower_buffer_addresses_ex, flags
 *                  SAFE|UBO-table: buffers read through physical pointers from the address table at binding 640, a
 *                  UNIFORM_BUFFER_DYNAMIC, vertex set 0 / fragment set 1), the translator library given by -l <path>.
 *   cc -O2 -o /tmp/replay tests/vk_capture_replay.c -lvulkan -ldl
 *   /tmp/replay <capture dir> [desc|table] [-l libnvmtl_translate.so] [-o out.ppm]
 * Exit 0 = the GPU's pixels match the CPU's (the draw is right on this machine), 1 = they differ (the bad pixels are listed).
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

static VkDevice dev; static VkPhysicalDevice pd; static VkPhysicalDeviceMemoryProperties mp; static VkQueue queue; static VkCommandPool pool;
static uint32_t mtype(uint32_t bits, VkMemoryPropertyFlags f)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
    printf("no memory type\n"); exit(3);
}
static void *slurp(const char *dir, const char *name, size_t *n)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc((size_t)l + 1); if (fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); ((char *)b)[l] = 0; if (n) *n = (size_t)l; return b;
}
typedef struct { char slot[8]; size_t off, len; uint64_t addr; uint8_t *bytes; size_t blo, bn; int owner; } binding;
typedef struct { VkBuffer b; VkDeviceMemory m; void *map; VkDeviceAddress va; } gbuf;
static gbuf mkbuf(size_t size, VkBufferUsageFlags u)
{
    gbuf g = {0};
    VkBufferCreateInfo bc = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, u | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_SHARING_MODE_EXCLUSIVE };
    CK(vkCreateBuffer(dev, &bc, 0, &g.b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, g.b, &mr);
    VkMemoryAllocateFlagsInfo fi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi, mr.size,
        mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CK(vkAllocateMemory(dev, &ma, 0, &g.m)); CK(vkBindBufferMemory(dev, g.b, g.m, 0)); CK(vkMapMemory(dev, g.m, 0, VK_WHOLE_SIZE, 0, &g.map));
    VkBufferDeviceAddressInfo ai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, g.b }; g.va = vkGetBufferDeviceAddress(dev, &ai);
    return g;
}
static void retarget_set(uint32_t *w, size_t nw, uint32_t set)
{
    for (size_t i = 5; i < nw;) { uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n) return;
        if (op == 71 && n >= 4 && w[i + 2] == 34) w[i + 3] = set;
        i += n; }
}
typedef int (*lower_ex_fn)(const uint8_t *, size_t, uint32_t, uint8_t **, size_t *, uint32_t *, char *, size_t);
static VkShaderModule module(const void *code, size_t n)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, n, code }; VkShaderModule m; CK(vkCreateShaderModule(dev, &ci, 0, &m)); return m;
}
static float rdf(const binding *b, size_t at) { float f; memcpy(&f, b->bytes + (at - b->blo), 4); return f; }

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: %s <capture dir> [desc|table] [-l lib] [-o out.ppm]\n", argv[0]); return 2; }
    const char *dir = argv[1], *mode = "desc", *lib = NULL, *outp = NULL;
    for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "-l") && i + 1 < argc) lib = argv[++i]; else if (!strcmp(argv[i], "-o") && i + 1 < argc) outp = argv[++i]; else mode = argv[i]; }
    const int table = !strcmp(mode, "table");
    char *meta = slurp(dir, "meta.txt", NULL); if (!meta) { printf("no meta.txt in %s\n", dir); return 2; }
    unsigned W = 0, H = 0, pitch = 0, inst = 0, vc = 0; float vp[6] = {0}; unsigned sc[4] = {0}; int hasSC = 0;
    char *p;
    if ((p = strstr(meta, "\ntarget "))) sscanf(p, "\ntarget %ux%u", &W, &H);
    if ((p = strstr(meta, "bytes_per_row="))) sscanf(p, "bytes_per_row=%u", &pitch);
    if ((p = strstr(meta, " count="))) sscanf(p, " count=%u instances=%u", &vc, &inst);
    if ((p = strstr(meta, "\nviewport set="))) sscanf(p, "\nviewport set=%*d %f %f %f %f %f %f", &vp[0], &vp[1], &vp[2], &vp[3], &vp[4], &vp[5]);
    if ((p = strstr(meta, "\nscissor set="))) sscanf(p, "\nscissor set=%d %u %u %u %u", &hasSC, &sc[0], &sc[1], &sc[2], &sc[3]);
    if (!pitch) pitch = ((W * 4 + 255) / 256) * 256;
    if (!hasSC) { sc[0] = sc[1] = 0; sc[2] = W; sc[3] = H; }
    printf("capture %s: target %ux%u pitch %u, %u instances of %u vertices, viewport %g %g %g %g, scissor %u %u %u %u, mode %s\n",
           dir, W, H, pitch, inst, vc, vp[0], vp[1], vp[2], vp[3], sc[0], sc[1], sc[2], sc[3], mode);
    /* bindings: "buffer v1: off=... len=... gpu_addr=0x..." ; bytes from <slot>.cpu_draw (u64 lo + bytes) */
    binding bd[16]; int nb = 0;
    for (p = meta; (p = strstr(p, "\nbuffer ")) && nb < 16; p++) {
        binding *b = &bd[nb]; memset(b, 0, sizeof *b); unsigned long long a = 0, off = 0, len = 0;
        if (sscanf(p, "\nbuffer %7[^:]: off=%llu len=%llu", b->slot, &off, &len) != 3) continue;
        char *g = strstr(p, "gpu_addr=0x"); if (g) sscanf(g, "gpu_addr=0x%llx", &a);
        b->off = off; b->len = len; b->addr = a;
        char nm[32]; snprintf(nm, sizeof nm, "%s.cpu_draw", b->slot); size_t n = 0; uint8_t *raw = slurp(dir, nm, &n);
        if (raw && n >= 8) { memcpy(&b->blo, raw, 8); b->bytes = raw + 8; b->bn = n - 8; b->owner = 1; }
        nb++;
    }
    for (int i = 0; i < nb; i++) if (!bd[i].bytes) for (int j = 0; j < nb; j++)
        if (bd[j].bytes && bd[j].addr == bd[i].addr) { bd[i].bytes = bd[j].bytes; bd[i].blo = bd[j].blo; bd[i].bn = bd[j].bn; }
    binding *v1 = 0, *v2 = 0, *f1 = 0, *f9 = 0;
    for (int i = 0; i < nb; i++) { binding *b = &bd[i];
        if (!strcmp(b->slot, "v1")) v1 = b;
        else if (!strcmp(b->slot, "v2")) v2 = b;
        else if (!strcmp(b->slot, "f1")) f1 = b;
        else if (!strcmp(b->slot, "f9")) f9 = b; }
    if (!v1 || !v2 || !f1 || !f9 || !v1->bytes || !v2->bytes || !f1->bytes || !f9->bytes) { printf("a buffer_image_blit capture needs v1, v2, f1, f9 with bytes\n"); return 2; }

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "replay", 1, "x", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ic = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai }; VkInstance in; CK(vkCreateInstance(&ic, 0, &in));
    uint32_t n1 = 1; vkEnumeratePhysicalDevices(in, &n1, &pd); vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); printf("device: %s, driver 0x%x\n", pp.deviceName, pp.driverVersion);
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.storageBuffer8BitAccess = 1; f12.uniformAndStorageBuffer8BitAccess = 1; f12.shaderInt8 = 1; f12.bufferDeviceAddress = 1; f12.scalarBlockLayout = 1;
    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12 };
    f11.storageBuffer16BitAccess = 1;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11 }; f2.features.shaderInt16 = 1; f2.features.shaderInt64 = 1;
    const char *exts[] = { "VK_EXT_image_drm_format_modifier" };
    float pr = 1; VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &pr };
    VkDeviceCreateInfo dc = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &q, 0, 0, 1, exts };
    CK(vkCreateDevice(pd, &dc, 0, &dev)); vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, 0 }; CK(vkCreateCommandPool(dev, &cpi, 0, &pool));

    /* one GPU buffer per distinct captured buffer, filled with its cpu_draw bytes at the same offsets */
    gbuf G[16]; int gi[16];
    for (int i = 0; i < nb; i++) {
        gi[i] = i;
        for (int j = 0; j < i; j++) if (bd[j].addr == bd[i].addr) { gi[i] = gi[j]; break; }
        if (gi[i] != i) continue;
        G[i] = mkbuf(bd[i].len, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        memset(G[i].map, 0, bd[i].len); if (bd[i].bytes) memcpy((uint8_t *)G[i].map + bd[i].blo, bd[i].bytes, bd[i].bn);
    }
    /* the target: B8G8R8A8_UNORM, linear, explicit row pitch, its memory a buffer we read back directly */
    VkSubresourceLayout pl = { 0, (VkDeviceSize)pitch * H, pitch, 0, 0 };
    VkImageDrmFormatModifierExplicitCreateInfoEXT ex = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, 0, 0 /* DRM_FORMAT_MOD_LINEAR */, 1, &pl };
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ex, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, { W, H, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img; CK(vkCreateImage(dev, &ici, 0, &img));
    VkMemoryRequirements imr; vkGetImageMemoryRequirements(dev, img, &imr);
    VkMemoryAllocateInfo ima = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, imr.size, mtype(imr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) };
    VkDeviceMemory imem; CK(vkAllocateMemory(dev, &ima, 0, &imem)); CK(vkBindImageMemory(dev, img, imem, 0));
    uint8_t *pix; CK(vkMapMemory(dev, imem, 0, VK_WHOLE_SIZE, 0, (void **)&pix));
    VkImageSubresource isr = { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 }; VkSubresourceLayout got; vkGetImageSubresourceLayout(dev, img, &isr, &got);
    printf("target layout: rowPitch %llu (asked %u), size %llu\n", (unsigned long long)got.rowPitch, pitch, (unsigned long long)got.size);
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, {0}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view; CK(vkCreateImageView(dev, &vci, 0, &view));

    /* shaders */
    size_t vn = 0, fn = 0; uint32_t *vs = slurp(dir, "vertex.spv", &vn), *fs = slurp(dir, "fragment.spv", &fn);
    if (!vs || !fs) { printf("no vertex.spv / fragment.spv\n"); return 2; }
    if (table) {
        void *h = dlopen(lib ? lib : "libnvmtl_translate.so", RTLD_NOW); if (!h) { printf("dlopen: %s\n", dlerror()); return 2; }
        lower_ex_fn lower = (lower_ex_fn)dlsym(h, "nvmtl_lower_buffer_addresses_ex"); if (!lower) { printf("no nvmtl_lower_buffer_addresses_ex\n"); return 2; }
        uint8_t *o; size_t on; uint32_t info[4]; char err[256];
        if (lower((uint8_t *)vs, vn, 1 | 2, &o, &on, info, err, sizeof err)) { printf("lower vertex: %s\n", err); return 2; } vs = (uint32_t *)o; vn = on;
        if (lower((uint8_t *)fs, fn, 1 | 2, &o, &on, info, err, sizeof err)) { printf("lower fragment: %s\n", err); return 2; } fs = (uint32_t *)o; fn = on;
    }
    retarget_set(fs, fn / 4, 1);
    VkShaderModule vm = module(vs, vn), fm = module(fs, fn);

    /* descriptor sets: set 0 vertex, set 1 fragment; desc mode: SSBOs at the Metal indices; table mode: the UBO table at 640 */
    VkDescriptorSetLayout sl[2]; VkDescriptorSet ds[2];
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 2, 2, ps }; VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, 0, &dp));
    gbuf tab = mkbuf(2 * 4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    for (int s = 0; s < 2; s++) {
        VkDescriptorSetLayoutBinding lb[2]; uint32_t nl = 0; VkShaderStageFlags st = s ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        if (table) lb[nl++] = (VkDescriptorSetLayoutBinding){ 640, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, st, 0 };
        else { lb[nl++] = (VkDescriptorSetLayoutBinding){ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st, 0 };
               lb[nl++] = (VkDescriptorSetLayoutBinding){ s ? 9u : 2u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st, 0 }; }
        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, nl, lb }; CK(vkCreateDescriptorSetLayout(dev, &lci, 0, &sl[s]));
        VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 1, &sl[s] }; CK(vkAllocateDescriptorSets(dev, &dai, &ds[s]));
        binding *a = s ? f1 : v1, *b = s ? f9 : v2;
        int ia = (int)(a - bd), ib = (int)(b - bd);
        if (table) {   /* the plugin's table: entry i = address + offset of Metal buffer i (32 slots), then 32 self pointers, textures */
            uint64_t *t = (uint64_t *)((uint8_t *)tab.map + (size_t)s * 4096);
            for (int k = 0; k < 64; k++) t[k] = 0;
            t[s ? 1 : 1] = G[gi[ia]].va + a->off; t[s ? 9 : 2] = G[gi[ib]].va + b->off;
            for (int k = 0; k < 32; k++) t[32 + k] = tab.va + (uint64_t)s * 4096 + (uint64_t)k * 8;
            VkDescriptorBufferInfo bi = { tab.b, (VkDeviceSize)s * 4096, 4096 };
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds[s], 640, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 0, &bi, 0 };
            vkUpdateDescriptorSets(dev, 1, &w, 0, 0);
        } else {
            VkDescriptorBufferInfo bi[2] = { { G[gi[ia]].b, a->off, VK_WHOLE_SIZE }, { G[gi[ib]].b, b->off, VK_WHOLE_SIZE } };
            VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds[s], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &bi[0], 0 },
                                          { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds[s], s ? 9u : 2u, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &bi[1], 0 } };
            vkUpdateDescriptorSets(dev, 2, w, 0, 0);
        }
    }
    VkPipelineLayoutCreateInfo plc = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 2, sl }; VkPipelineLayout lay; CK(vkCreatePipelineLayout(dev, &plc, 0, &lay));
    VkPipelineShaderStageCreateInfo stg[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main" },
                                               { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main" } };
    VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ias = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkViewport vpt = { vp[0], vp[1], vp[2], vp[3], vp[4], vp[5] };
    if (vpt.width == 0) { vpt.width = (float)W; vpt.height = (float)H; vpt.maxDepth = 1; }
    VkViewport vflip = vpt; vflip.y = vpt.y + vpt.height; vflip.height = -vpt.height;   /* Metal's y-up NDC, as the plugin sets it */
    VkRect2D sci = { { (int32_t)sc[0], (int32_t)sc[1] }, { sc[2], sc[3] } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vflip, 1, &sci };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO }; rs.lineWidth = 1; rs.cullMode = VK_CULL_MODE_NONE;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { 1, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 0xf };
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &cba };
    VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo prc = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, 1, &cf };
    VkGraphicsPipelineCreateInfo gpc = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &prc, 0, 2, stg, &vis, &ias, 0, &vps, &rs, &ms, 0, &cbs, 0, lay };
    VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, 0, 1, &gpc, 0, &pipe));

    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 }; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &ca, &cb));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; CK(vkBeginCommandBuffer(cb, &bi));
    VkImageMemoryBarrier toc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &toc);
    VkClearColorValue black = {{ 0, 0, 0, 0 }}; VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    VkImageMemoryBarrier toa = toc; toa.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; toa.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    toa.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; toa.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, 0, 0, 0, 1, &toa);
    VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0, 0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, { { 0, 0 }, { W, H } }, 1, 0, 1, &att };
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    uint32_t dyn[2] = { 0, 0 };
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 2, ds, table ? 2u : 0u, dyn);
    vkCmdDraw(cb, vc, inst, 0, 0);
    vkCmdEndRendering(cb);
    VkImageMemoryBarrier toh = toa; toh.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; toh.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toh.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; toh.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, 0, 0, 0, 1, &toh);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb }; CK(vkQueueSubmit(queue, 1, &si, 0)); CK(vkQueueWaitIdle(queue));

    /* the CPU's answer: per pixel the last rect covering it (instances in order, blend over what is there) */
    float *ref = calloc((size_t)W * H * 4, sizeof(float));
    float m[16]; for (int k = 0; k < 16; k++) m[k] = rdf(v2, v2->off + 4 * (size_t)k);
    float col[4]; for (int k = 0; k < 4; k++) col[k] = rdf(f1, f1->off + 4 * (size_t)k);
    printf("colour uniform (%g %g %g %g), mvp col0 (%g %g) col1 (%g %g) col3 (%g %g)\n", col[0], col[1], col[2], col[3], m[0], m[1], m[4], m[5], m[12], m[13]);
    for (unsigned i = 0; i < inst; i++) {
        const size_t r = v1->off + 24 * (size_t)i;
        const float mnx = rdf(v1, r), mny = rdf(v1, r + 4), sw = rdf(v1, r + 8), sh = rdf(v1, r + 12), off = rdf(v1, r + 16), bpr = rdf(v1, r + 20);
        /* the quad's corners in pixels: (min.x + u, min.y - v), u in {0,sw}, v in {0,sh} */
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int c = 0; c < 4; c++) { const float X = mnx + ((c & 1) ? sw : 0), Y = mny - ((c & 2) ? sh : 0);
            const float nx = m[0] * X + m[4] * Y + m[12], ny = m[1] * X + m[5] * Y + m[13];
            const float px = vpt.x + (nx + 1) * vpt.width / 2, py = vpt.y + (1 - ny) * vpt.height / 2;
            x0 = fminf(x0, px); x1 = fmaxf(x1, px); y0 = fminf(y0, py); y1 = fmaxf(y1, py); }
        for (int py = (int)floorf(y0); py <= (int)ceilf(y1); py++) for (int px = (int)floorf(x0); px <= (int)ceilf(x1); px++) {
            if (px < (int)sc[0] || py < (int)sc[1] || px >= (int)(sc[0] + sc[2]) || py >= (int)(sc[1] + sc[3]) || px < 0 || py < 0 || px >= (int)W || py >= (int)H) continue;
            const float cx = px + 0.5f, cy = py + 0.5f;
            if (cx <= x0 || cx > x1 || cy <= y0 || cy > y1) continue;
            const float u = (cx - x0) / (x1 - x0) * sw, v = (cy - y0) / (y1 - y0) * sh;   /* uv grows right and down from (min.x, min.y) */
            const float fx = fminf(floorf(u), sw - 1), fy = fminf(floorf(v), sh - 1);
            const long at = (long)(fx + off + fy * bpr);
            const size_t a = f9->off + (size_t)at; if (at < 0 || a < f9->blo || a >= f9->blo + f9->bn) continue;
            const float cov = (float)f9->bytes[a - f9->blo];
            float *d = ref + ((size_t)py * W + px) * 4;
            for (int k = 0; k < 4; k++) { const float s = cov * col[k]; d[k] = s + d[k] * (1 - cov * col[3]); }
        }
    }
    unsigned bad = 0, lit = 0, shown = 0; int bx0 = 1 << 30, bx1 = -1;
    for (unsigned y = sc[1]; y < sc[1] + sc[3] && y < H; y++) for (unsigned x = sc[0]; x < sc[0] + sc[2] && x < W; x++) {
        const uint8_t *g = pix + (size_t)y * got.rowPitch + 4 * (size_t)x; const float *d = ref + ((size_t)y * W + x) * 4;
        const int e[4] = { (int)lrintf(fminf(1, fmaxf(0, d[2])) * 255), (int)lrintf(fminf(1, fmaxf(0, d[1])) * 255), (int)lrintf(fminf(1, fmaxf(0, d[0])) * 255), (int)lrintf(fminf(1, fmaxf(0, d[3])) * 255) };
        int diff = 0; for (int k = 0; k < 4; k++) if (abs(e[k] - g[k]) > 2) diff = 1;
        if (g[3]) lit++;
        if (diff) { bad++; if ((int)x < bx0) bx0 = (int)x; if ((int)x > bx1) bx1 = (int)x;
            if (shown++ < 8) printf("  pixel %u,%u: GPU bgra %u %u %u %u, CPU %d %d %d %d\n", x, y, g[0], g[1], g[2], g[3], e[0], e[1], e[2], e[3]); }
    }
    if (outp) { FILE *f = fopen(outp, "wb"); if (f) { fprintf(f, "P6\n%u %u\n255\n", W, H);
        for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) { const uint8_t *g = pix + (size_t)y * got.rowPitch + 4 * (size_t)x; uint8_t c3[3] = { g[2], g[1], g[0] }; fwrite(c3, 1, 3, f); }
        fclose(f); } }
    printf("%s: %u of %u scissor pixels differ from the CPU evaluation (columns %d..%d), %u lit by the GPU\n", bad ? "DIFFERENT" : "SAME", bad, sc[2] * sc[3], bad ? bx0 : -1, bad ? bx1 : -1, lit);
    return bad != 0;
}
