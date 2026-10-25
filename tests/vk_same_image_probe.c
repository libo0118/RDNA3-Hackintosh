/* Probe, not a unit test (needs a Vulkan device, e.g. the host's RADV; not in `make test`): a copy with ONE image on both sides, the way
 * metal/plugin/nvmtl_vk.c nvmtl_vk_cmd_copy_image_sub3 records it (DRIVER-PLAN N50). A scroll by blitting a window texture onto itself
 * (overlapping rows); a texture-to-texture blit encoder copy can also name one texture on both sides.
 *   mode 0 (NEW, what the plugin does now): both sides in GENERAL when the regions share no texel; a scratch image when they do
 *          (image -> scratch -> image); the image ends in SHADER_READ_ONLY.
 *   mode 1 (OLD, the bug): the image goes to TRANSFER_SRC and then to TRANSFER_DST and vkCmdCopyImage declares TRANSFER_SRC for the source while the
 *          image rests in TRANSFER_DST; overlapping regions are copied in place (undefined in Vulkan). Informative: RADV may or may not tolerate it.
 *   mode 2 (control): GENERAL on both sides, overlapping regions copied in place. Informative: what the undefined overlap does on this driver.
 * A CPU model of the image is kept (a copy reads the whole source region before it writes: memmove semantics); after every command buffer of BATCH operations
 * (copies of overlapping and of disjoint rectangles, scroll-like shifts by N rows or columns, and uploads of fresh rectangles) the image is read back and every pixel must
 * match. The image is 1024x768 RGBA8 with colour-attachment usage, so DCC applies (RADV_DEBUG=nodcc turns it off: must agree), and its memory is first
 * filled with 0xCD by an earlier image so stale metadata is garbage.
 *   cc -O1 -o /tmp/same_image_probe tests/vk_same_image_probe.c -lvulkan && /tmp/same_image_probe        (exit 0 = mode 0 right; modes 1 / 2 only print)
 * Env: SEED=n (1), OPS=n (400), BATCH=n (operations per command buffer, 1), MODE=n (only that mode), OVL=0|1 (only disjoint / only overlapping copies), DEV=n (physical device index, default: the first discrete GPU).
 * SPDX-License-Identifier: MIT */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)
#define W 1024u
#define H 768u

static VkDevice dev; static VkPhysicalDevice pd; static VkPhysicalDeviceMemoryProperties mp; static VkQueue queue; static uint32_t qfam; static VkCommandPool pool;
static uint32_t mtype(uint32_t bits, VkMemoryPropertyFlags f) { for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i; printf("no memory type\n"); exit(3); }
static uint64_t g_rng;
static uint32_t rnd(void) { g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(g_rng >> 33); }
static uint32_t hashpix(uint32_t x, uint32_t y, uint32_t s) { uint32_t h = x * 0x9E3779B1u ^ (y + 0x7F4A7C15u) * 0x85EBCA77u ^ s * 0xC2B2AE3Du; h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; return h | 0xFF000000u; }

typedef struct { VkImage img; VkDeviceMemory mem; uint32_t w, h; VkImageLayout lay; } im_t;
static VkCommandBuffer begincb(void)
{ VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 }; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &ca, &cb));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }; CK(vkBeginCommandBuffer(cb, &bi)); return cb; }
static void endcb(VkCommandBuffer cb)
{ CK(vkEndCommandBuffer(cb)); VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb }; CK(vkQueueSubmit(queue, 1, &si, 0)); CK(vkQueueWaitIdle(queue)); vkFreeCommandBuffers(dev, pool, 1, &cb); }
static void mkimg(uint32_t w, uint32_t h, im_t *im)
{
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { w, h, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED };
    CK(vkCreateImage(dev, &ic, 0, &im->img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im->img, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CK(vkAllocateMemory(dev, &ai, 0, &im->mem)); CK(vkBindImageMemory(dev, im->img, im->mem, 0));
    im->w = w; im->h = h; im->lay = VK_IMAGE_LAYOUT_UNDEFINED;
}
static void killimg(im_t *im) { vkDestroyImage(dev, im->img, 0); vkFreeMemory(dev, im->mem, 0); memset(im, 0, sizeof *im); }
/* the plugin's img_to: nothing when the image already rests there */
static void img_to(VkCommandBuffer cb, im_t *im, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    if (im->lay == to) return;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, sa, da, im->lay, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im->img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, 0, 0, 0, 1, &b); im->lay = to;
}
static void membar(VkCommandBuffer cb)
{ VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT };
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, 0, 0, 0); }

static VkBuffer stage; static VkDeviceMemory stagem; static uint32_t *stagep;   /* W*H*4 */
static void mkstage(void)
{
    VkBufferCreateInfo bc = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, (VkDeviceSize)W * H * 4 * 3, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE };
    CK(vkCreateBuffer(dev, &bc, 0, &stage)); VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, stage, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CK(vkAllocateMemory(dev, &ai, 0, &stagem)); CK(vkBindBufferMemory(dev, stage, stagem, 0)); CK(vkMapMemory(dev, stagem, 0, VK_WHOLE_SIZE, 0, (void **)&stagep));
}
/* replaceRegion-style upload of a rectangle (the staging holds it tightly packed at offset 0) */
static size_t g_stoff;   /* bytes of the staging used by this command buffer's uploads */
static void upload(VkCommandBuffer cb, im_t *im, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    img_to(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c = { g_stoff, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { (int32_t)x, (int32_t)y, 0 }, { w, h, 1 } };
    vkCmdCopyBufferToImage(cb, stage, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    img_to(cb, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}
static void readback(im_t *im, uint32_t *out)
{
    VkCommandBuffer cb = begincb();
    img_to(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { im->w, im->h, 1 } };
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage, 1, &c);
    img_to(cb, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    endcb(cb); memcpy(out, stagep, (size_t)W * H * 4);
}
static int overlaps(int sx, int sy, int dx, int dy, int w, int h) { return sx < dx + w && dx < sx + w && sy < dy + h && dy < sy + h; }

/* nvmtl_vk_cmd_copy_image_sub3 for one image on both sides, three ways (see the top) */
static int g_scr_n, g_ovl = -1; static im_t g_scr[64];
static void same_copy(VkCommandBuffer cb, im_t *im, int mode, int sx, int sy, int dx, int dy, int w, int h)
{
    const int ov = overlaps(sx, sy, dx, dy, w, h);
    VkImageCopy r = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { sx, sy, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { dx, dy, 0 }, { (uint32_t)w, (uint32_t)h, 1 } };
    membar(cb);
    if (mode == 1) {
        img_to(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        img_to(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImage(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);   /* the layout lie */
    } else {
        img_to(cb, im, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (mode == 0 && ov) {
            /* like the plugin: one scratch image per command buffer, reused by every overlapping copy that fits, a larger one made when none does */
            im_t *s = NULL; uint32_t sw = (uint32_t)w, sh = (uint32_t)h;
            for (int i = 0; i < g_scr_n; i++) { if (g_scr[i].w >= (uint32_t)w && g_scr[i].h >= (uint32_t)h) { s = &g_scr[i]; break; } if (g_scr[i].w > sw) sw = g_scr[i].w; if (g_scr[i].h > sh) sh = g_scr[i].h; }
            if (!s) { s = &g_scr[g_scr_n++]; mkimg(sw, sh, s); }
            img_to(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageCopy in = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { sx, sy, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { (uint32_t)w, (uint32_t)h, 1 } };
            vkCmdCopyImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &in);
            img_to(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageCopy out = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { dx, dy, 0 }, { (uint32_t)w, (uint32_t)h, 1 } };
            vkCmdCopyImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &out);
        } else vkCmdCopyImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    }
    img_to(cb, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    membar(cb);
}

static int run_mode(int mode, uint64_t seed, int ops, int batch, uint64_t *nov, uint64_t *ndis, uint64_t *nbadpix)
{
    g_rng = seed * 7919 + (uint64_t)mode;
    { im_t junk; mkimg(W, H, &junk); VkCommandBuffer cb = begincb(); VkClearColorValue cc; memset(&cc, 0xCD, sizeof cc);   /* dirty VRAM for the subject */
      VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }; img_to(cb, &junk, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      vkCmdClearColorImage(cb, junk.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rg); endcb(cb); killimg(&junk); }
    im_t im; mkimg(W, H, &im);
    uint32_t *model = malloc((size_t)W * H * 4), *got = malloc((size_t)W * H * 4), *tmp = malloc((size_t)W * H * 4);
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) model[y * W + x] = stagep[y * W + x] = hashpix(x, y, 1);
    { g_stoff = 0; VkCommandBuffer cb = begincb(); upload(cb, &im, 0, 0, W, H); endcb(cb); }
    int bad = 0;
    for (int done = 0; done < ops && bad < 3; ) {
        VkCommandBuffer cb = begincb(); g_scr_n = 0; g_stoff = 0;
        for (int k = 0; k < batch && done < ops; k++, done++) {
            int kind = (int)(rnd() % 8);
            if (g_ovl == 0 && kind < 4) kind = 4 + kind % 3;           /* OVL=0: disjoint copies and uploads only */
            if (g_ovl == 1 && kind >= 4 && kind < 7) kind -= 4;       /* OVL=1: overlapping scrolls and uploads only */
            int w = 16 + (int)(rnd() % 300), h = 16 + (int)(rnd() % 200), sx = (int)(rnd() % (W - (uint32_t)w)), sy = (int)(rnd() % (H - (uint32_t)h)), dx, dy;
            if (kind == 7) {   /* an upload of fresh bytes (the app drew something new) */
                { uint32_t *sp = stagep + g_stoff / 4; for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) sp[y * w + x] = model[(sy + y) * W + sx + x] = hashpix((uint32_t)x, (uint32_t)y, (uint32_t)rnd()); }
                upload(cb, &im, (uint32_t)sx, (uint32_t)sy, (uint32_t)w, (uint32_t)h); g_stoff += (size_t)w * h * 4; continue; }
            if (kind < 4) {   /* a scroll: the same columns, N rows up or down (or the same rows, N columns), overlapping */
                const int n = 1 + (int)(rnd() % 40);
                if (kind == 0)      { sy = (int)(rnd() % (H - (uint32_t)h - (uint32_t)n + 1)); dx = sx; dy = sy + n; }
                else if (kind == 1) { sy = n + (int)(rnd() % (H - (uint32_t)h - (uint32_t)n + 1)); dx = sx; dy = sy - n; }
                else if (kind == 2) { sx = (int)(rnd() % (W - (uint32_t)w - (uint32_t)n + 1)); dx = sx + n; dy = sy; }
                else                { sx = n + (int)(rnd() % (W - (uint32_t)w - (uint32_t)n + 1)); dx = sx - n; dy = sy; }
            } else { do { dx = (int)(rnd() % (W - (uint32_t)w)); dy = (int)(rnd() % (H - (uint32_t)h)); } while (overlaps(sx, sy, dx, dy, w, h)); }   /* disjoint */
            if (overlaps(sx, sy, dx, dy, w, h)) (*nov)++; else (*ndis)++;
            same_copy(cb, &im, mode, sx, sy, dx, dy, w, h);
            for (int y = 0; y < h; y++) memcpy(tmp + y * w, model + (sy + y) * W + sx, (size_t)w * 4);   /* memmove semantics */
            for (int y = 0; y < h; y++) memcpy(model + (dy + y) * W + dx, tmp + y * w, (size_t)w * 4);
        }
        endcb(cb);
        for (int i = 0; i < g_scr_n; i++) killimg(&g_scr[i]);
        readback(&im, got);
        uint64_t wrong = 0; uint32_t fx = 0, fy = 0;
        for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) if (got[y * W + x] != model[y * W + x]) { if (!wrong) { fx = x; fy = y; } wrong++; }
        if (wrong) { bad++; *nbadpix += wrong; printf("  mode %d: after op %d, %llu wrong pixel(s), the first at (%u,%u): got %08x want %08x\n", mode, done, (unsigned long long)wrong, fx, fy, got[fy * W + fx], model[fy * W + fx]);
                     memcpy(model, got, (size_t)W * H * 4); }   /* go on from what the image holds */
    }
    killimg(&im); free(model); free(got); free(tmp);
    return bad;
}

int main(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "same_image_probe", 1, 0, 0, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ic = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst; CK(vkCreateInstance(&ic, 0, &inst));
    uint32_t n = 0; CK(vkEnumeratePhysicalDevices(inst, &n, 0)); VkPhysicalDevice pds[16]; if (n > 16) n = 16; CK(vkEnumeratePhysicalDevices(inst, &n, pds));
    const char *de = getenv("DEV"); int pick = -1;
    for (uint32_t i = 0; i < n; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); printf("device %u: %s\n", i, p.deviceName);
        if (pick < 0 && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pick = (int)i; }
    if (de) pick = atoi(de);
    if (pick < 0) pick = 0;
    if ((uint32_t)pick >= n) { printf("no device\n"); return 3; }
    pd = pds[pick]; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p); printf("using %s\n", p.deviceName); }
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, 0); VkQueueFamilyProperties qp[16]; if (qn > 16) qn = 16; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp);
    for (qfam = 0; qfam < qn; qfam++) if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    float prio = 1; VkDeviceQueueCreateInfo qc = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, qfam, 1, &prio };
    VkDeviceCreateInfo dc = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qc };
    CK(vkCreateDevice(pd, &dc, 0, &dev)); vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pc = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam }; CK(vkCreateCommandPool(dev, &pc, 0, &pool));
    mkstage();
    const char *e;
    const int ops = (e = getenv("OPS")) ? atoi(e) : 400, batch = (e = getenv("BATCH")) ? atoi(e) : 1, only = (e = getenv("MODE")) ? atoi(e) : -1;
    const uint64_t seed = (e = getenv("SEED")) ? strtoull(e, 0, 10) : 1;
    int rc = 0; if ((e = getenv("OVL"))) g_ovl = atoi(e);
    for (int mode = 0; mode < 3; mode++) {
        if (only >= 0 && mode != only) continue;
        uint64_t nov = 0, ndis = 0, np = 0;
        const int bad = run_mode(mode, seed, ops, batch, &nov, &ndis, &np);
        printf("mode %d (%s): %d ops (%llu overlapping, %llu disjoint copies), %s%s\n", mode, mode == 0 ? "NEW: GENERAL / scratch image" : mode == 1 ? "OLD: SRC then DST layout, SRC declared" : "control: GENERAL in place",
               ops, (unsigned long long)nov, (unsigned long long)ndis, bad ? "WRONG PIXELS in " : "every pixel right", bad ? "some readbacks" : "");
        if (mode == 0 && bad) rc = 1;
    }
    printf("%s\n", rc ? "FAIL: the plugin's new same-image copy is wrong on this driver" : "ok");
    return rc;
}
