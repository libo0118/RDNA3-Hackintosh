/*
 * NVMTLTrace.m - the Objective-C half of the plugin's frame trace (nvmtl_trace.h has the file format and the emitters;
 * tools/plugin_trace.py decodes it). Imported by NVMTLDevice.m after NVMTLObjects.h, before the classes.
 *
 * Rules: read state the plugin already has, take no IOSurface lock, call nothing that can fail or block, change no
 * decision. Every function returns at once when tracing is off (nvt_on() is one atomic load).
 */

static inline NVMTLTexture *nvmtl_tr_root(NVMTLTexture *t) { return (t && t->_parent) ? t->_parent : t; }

/* one id per command buffer for the life of the process; 0 when tracing is off */
static uint32_t nvmtl_tr_cb_id(NVMTLCommandBuffer *cb)
{
    if (!cb || !nvt_on()) return 0;
    if (!cb->_trId) cb->_trId = nvt_next_id();
    return cb->_trId;
}

/* the band hash of the CPU memory of a surface plane the caller has locked (or, with NVMTL_TRACE_HASH=2, not locked).
 * NO when the surface is not a 4-byte-per-pixel picture big enough to be worth it. */
static BOOL nvmtl_tr_hash(NVMTLTexture *rt, const void *base, size_t bpr, uint32_t bands[8])
{
    if (!base || nvt_hash_mode() == 0) return NO;
    if (rt->_i.bpp != 4 || rt->_i.w < 64 || rt->_i.h < 32 || bpr < (size_t)rt->_i.w * 4) return NO;
    nvt_band_hash(base, bpr, rt->_i.w, rt->_i.h, bands);
    return YES;
}

/* SURF_IN / SURF_OUT of a root surface texture. t0: nvt_now() at the start; bands: from nvmtl_tr_hash or NULL */
static void nvmtl_tr_surf(NVMTLTexture *rt, unsigned kind, unsigned path, int rc, uint32_t seed0, uint32_t seed1,
                          uint64_t t0, uint64_t bytes, const uint32_t *bands, unsigned flags)
{
    if (!rt || !rt->_surf || !nvt_on()) return;
    nvt_ev_surf(kind, path, (uint32_t)IOSurfaceGetID(rt->_surf), (unsigned)rt->_plane, rt->_i.w, rt->_i.h, (uint32_t)rt->_fmt,
                (uint32_t)IOSurfaceGetBytesPerRowOfPlane(rt->_surf, rt->_plane), flags | (rt->_vramOn ? NVT_F_VRAM : 0u),
                rc, seed0, seed1, t0, bytes, bands);
}

/* a surface got its texture: family VRAM, or a private image and why (g_tr_why, set by nvmtl_surface_image_create) */
static void nvmtl_tr_map(NVMTLTexture *rt, unsigned why)
{
    if (!rt || !rt->_surf || !nvt_on()) return;
    nvt_ev_map((uint32_t)IOSurfaceGetID(rt->_surf), (unsigned)rt->_plane, rt->_i.w, rt->_i.h, (uint32_t)rt->_fmt,
               (uint32_t)IOSurfaceGetBytesPerRowOfPlane(rt->_surf, rt->_plane), rt->_vramOn ? 1 : 0, why, rt->_surfSynced ? 1 : 0);
}

/* renderCommandEncoderWithDescriptor: one record per colour attachment that is an IOSurface texture; remembers the
 * pass in the encoder so that endEncoding can say what it turned out to be. Returns the pass id (0: no surface). */
static uint32_t nvmtl_tr_pass_begin(NVMTLCommandBuffer *cb, NVMTLRenderCommandEncoder *e, MTLRenderPassDescriptor *d)
{
    if (!nvt_on()) return 0;
    nvt_count(NVT_C_PASSES, 1);
    unsigned natt = 0;
    for (NSUInteger i = 0; i < NVMTL_NCOL; i++) {
        id tx = d.colorAttachments[i].texture;
        if (tx && [tx isKindOfClass:[NVMTLTexture class]] && nvmtl_tr_root((NVMTLTexture *)tx)->_surf) natt++;
    }
    if (!natt) return 0;
    const uint32_t pass = nvt_next_id();
    const uint32_t cbid = nvmtl_tr_cb_id(cb);
    const unsigned extra = (d.depthAttachment.texture ? 1u : 0u);
    unsigned att = 0;
    for (NSUInteger i = 0; i < NVMTL_NCOL; i++) {
        MTLRenderPassColorAttachmentDescriptor *ca = d.colorAttachments[i];
        id tx = ca.texture;
        if (!tx || ![tx isKindOfClass:[NVMTLTexture class]]) continue;
        NVMTLTexture *t = (NVMTLTexture *)tx, *rt = nvmtl_tr_root(t);
        if (!rt->_surf) continue;
        const uint32_t sid = (uint32_t)IOSurfaceGetID(rt->_surf);
        if (!att) { e->_trSurf = sid; e->_trW = rt->_i.w; e->_trH = rt->_i.h; }
        MTLClearColor cc = ca.clearColor;
        const float clear[4] = { (float)cc.red, (float)cc.green, (float)cc.blue, (float)cc.alpha };
        nvt_ev_pass_begin(cbid, pass, att, natt, sid, (unsigned)rt->_plane, rt->_i.w, rt->_i.h, (uint32_t)rt->_fmt,
                          (t != rt ? NVT_F_VIEW : 0u) | (rt->_vramOn ? NVT_F_VRAM : 0u), (uint32_t)ca.loadAction,
                          (uint32_t)ca.storeAction, extra | (ca.resolveTexture ? 2u : 0u), clear);
        att++;
    }
    e->_trPass = pass;
    e->_trSets0 = cb ? cb->_c.nsets : 0;
    e->_trT0 = nvt_now();
    return pass;
}

/* endEncoding of a traced pass: path NVT_PASS_*, rc nonzero when the clear / empty-pass call failed */
static void nvmtl_tr_pass_end(NVMTLRenderCommandEncoder *e, unsigned path, int rc)
{
    if (!e->_trPass) return;
    const uint32_t now = e->_cb ? e->_cb->_c.nsets : 0;
    nvt_ev_pass_end(nvmtl_tr_cb_id(e->_cb), e->_trPass, path, rc, e->_trSurf, e->_trW, e->_trH,
                    now >= e->_trSets0 ? now - e->_trSets0 : 0u, e->_clear, e->_trT0);
    e->_trPass = 0;
}


/* ---- the encode-time split of a render encoder (nvt_ev_enc_split) ----
 * Per thread: an encoder is encoded by one thread. t0 != 0 only between a render encoder's creation and its endEncoding, so
 * the surface-in checks of blit and compute encoders are not counted. With the trace off nvmtl_split_now() is 0 and every
 * add is a branch. Reads clocks only; changes no decision. */
typedef struct { uint64_t t0, pipe, desc, sin, pipe_max, desc_max; uint32_t draws, builds, sin_calls; } nvmtl_split;
static __thread nvmtl_split g_split;
static inline uint64_t nvmtl_split_now(void) { return g_split.t0 ? nvt_now() : 0; }
static inline void nvmtl_split_add(uint64_t t0, uint64_t *sum, uint64_t *mx)
{
    if (!t0) return;
    const uint64_t now = nvt_now(), d = now > t0 ? now - t0 : 0;
    *sum += d;
    if (mx && d > *mx) *mx = d;
}
static void nvmtl_split_begin(void)
{
    if (!nvt_on()) return;
    memset(&g_split, 0, sizeof g_split);
    g_split.t0 = nvt_now();
}
static inline uint32_t nvmtl_split_us(uint64_t ticks)
{
    const uint64_t us = nvt_ticks_to_ns(ticks) / 1000u;
    return us > 0xffffffffull ? 0xffffffffu : (uint32_t)us;
}
static void nvmtl_split_end(NVMTLRenderCommandEncoder *e)
{
    if (!g_split.t0) return;
    const nvmtl_split s = g_split;
    memset(&g_split, 0, sizeof g_split);
    if (!s.draws) return;
    const uint64_t now = nvt_now();
    nvt_ev_enc_split(nvmtl_tr_cb_id(e->_cb), e->_trPass, s.draws, s.builds, nvmtl_split_us(now > s.t0 ? now - s.t0 : 0),
                     nvmtl_split_us(s.pipe), nvmtl_split_us(s.desc), nvmtl_split_us(s.sin), s.sin_calls,
                     nvmtl_split_us(s.pipe_max), nvmtl_split_us(s.desc_max));
}
