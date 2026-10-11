/*
 * dcn32_dp_mst.c - see dcn32_dp_mst.h. Display Core's MST stream handling
 * for DCN 3.2 (Linux v6.12, MIT); every function names the one it ports.
 * DC's link_mst_stream_allocation_table holds stream encoders; here the
 * stream encoder is the DIG front end index (ENGINE_ID_DIGA = 0, as the
 * DP_MSE_SAT_SRC field takes it).
 *
 * SPDX-License-Identifier: MIT
 */
#include "dcn32_dp_mst.h"
#include "fixpt31_32.h"
#include "gen/dcn_3_2_0_dp.h"
#include "gen/dcn_3_2_0_dsc.h"

#define GET(v, reg, field) (((v) & (uint32_t)reg##__##field##_MASK) >> reg##__##field##__SHIFT)
#define FIELD(reg, field, x) (((uint32_t)(x) << reg##__##field##__SHIFT) & (uint32_t)reg##__##field##_MASK)
#define DP_S   (regDP1_DP_LINK_CNTL - regDP0_DP_LINK_CNTL)

static uint32_t rd(rdna3_dev *d, uint32_t seg, uint32_t off)
{
    return rdna3_rreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off));
}

static void wr(rdna3_dev *d, uint32_t seg, uint32_t off, uint32_t v)
{
    rdna3_wreg(d, rdna3_reg_offset(d, HWIP_DCE, 0, (int)seg, off), v);
}

#define RI(d, reg, n) rd((d), reg##_BASE_IDX, (uint32_t)(reg) + DP_S * (n))
#define WI(d, reg, n, v) wr((d), reg##_BASE_IDX, (uint32_t)(reg) + DP_S * (n), (v))

/* DP_MST_UPDATE_MAX_RETRY */
#define DP_MST_UPDATE_MAX_RETRY 50
/* ACT_SUCCESS / ACT_LINK_LOST / ACT_FAILED */
enum { ACT_SUCCESS = 0, ACT_LINK_LOST, ACT_FAILED };
/* PEAK_FACTOR_X1000 */
#define PEAK_FACTOR_X1000 1006

/* link_dp_get_encoding_format == DP_UNKNOWN_ENCODING (8b/10b only here) */
static int link_is_unknown_encoding(const rdna3_dp_link_settings *s)
{
    return s->link_rate < RD_DP_LINK_RATE_LOW || s->link_rate > RD_DP_LINK_RATE_HIGH3;
}

/* enable_mst_on_sink */
void dcn32_dp_mst_enable_on_sink(rdna3_dp_link *l, int enable)
{
    uint8_t mstm_cntl = 0;
    dcaux_dpcd_read(l->d, l->aux, RD_DPCD_MSTM_CTRL, &mstm_cntl, 1);
    if (enable)
        mstm_cntl = (uint8_t)(mstm_cntl | RD_DP_MST_EN);
    else
        mstm_cntl = (uint8_t)(mstm_cntl & ~RD_DP_MST_EN);
    dcaux_dpcd_write(l->d, l->aux, RD_DPCD_MSTM_CTRL, &mstm_cntl, 1);
}

/* dm_mst_get_pbn_divider */
uint32_t dcn32_dp_mst_pbn_divider(const rdna3_dp_link *l)
{
    return dcn32_dp_link_bw_kbps(l, dcn32_dp_link_cap(l)) / (8 * 1000 * 54);
}

/* kbps_from_pbn (DC computes in uint64_t: above ~10000 PBN, which UHBR
 * hubs reach, a 32-bit product wraps - PR #2 review) */
static uint32_t kbps_from_pbn(uint32_t pbn)
{
    uint64_t kbps = pbn;
    kbps *= (1000000 / PEAK_FACTOR_X1000);
    kbps *= 8;
    kbps *= 54;
    kbps /= 64;
    return kbps > 0xffffffffull ? 0xffffffffu : (uint32_t)kbps;
}

int dcn32_dp_mst_mode_fits(rdna3_dp_link *l, uint32_t stream_kbps, uint32_t pixel_khz, uint32_t *pbn,
                           uint32_t *slots)
{
    rdna3_mst_mgr *mgr = l->mgr;
    rdna3_mst_port *port;
    uint32_t root_link_bw_in_kbps, virtual_channel_bw_in_kbps, end_to_end_bw_in_kbps, pbn_div;
    if (!mgr || l->mst_port < 0 || l->mst_port >= RD_MST_MAX_PORTS || !mgr->port[l->mst_port].used)
        return RDNA3_EINVAL;
    port = &mgr->port[l->mst_port];
    /* dm_dp_mst_is_port_support_mode, DSC unnecessary case: the end-to-end
     * bottleneck of the root link and the port's virtual channel */
    root_link_bw_in_kbps = dcn32_dp_link_bw_kbps(l, dcn32_dp_link_cap(l));
    virtual_channel_bw_in_kbps = kbps_from_pbn(port->full_pbn);
    end_to_end_bw_in_kbps = root_link_bw_in_kbps < virtual_channel_bw_in_kbps ? root_link_bw_in_kbps
                                                                               : virtual_channel_bw_in_kbps;
    if (stream_kbps > end_to_end_bw_in_kbps)
        return RDNA3_EINVAL;   /* DSC over MST is not ported */
    /* dm_encoder_helper_atomic_check: drm_dp_calc_pbn_mode(clock, bpp << 4),
     * drm_dp_atomic_find_time_slots with pbn_div = dm_mst_get_pbn_divider */
    *pbn = (uint32_t)drm_dp_calc_pbn_mode((int)pixel_khz, 24 << 4);
    pbn_div = dcn32_dp_mst_pbn_divider(l);
    if (!pbn_div)
        return RDNA3_EINVAL;
    *slots = (*pbn + pbn_div - 1) / pbn_div;
    if (*slots > (uint32_t)mgr->total_avail_slots)
        return RDNA3_EINVAL;   /* drm_dp_mst_atomic_check_payload_alloc_limits */
    return RDNA3_OK;
}

/* get_pbn_per_slot: the link's MBytes/s / 54 */
static rd_fixpt get_pbn_per_slot(const rdna3_dp_link *l)
{
    uint32_t link_rate_in_mbytes_per_sec = dcn32_dp_link_bw_kbps(l, &l->cur_link_settings);
    link_rate_in_mbytes_per_sec /= 8000;   /* kbits to MBytes */
    return dc_fixpt_div_int(dc_fixpt_from_int(link_rate_in_mbytes_per_sec), 54);
}

/* get_pbn_from_bw_in_kbps: kbps x 64 / (54 x 8 x 1000), no 1.006 margin
 * (get_pbn_per_slot has it) */
static rd_fixpt get_pbn_from_bw_in_kbps(uint64_t kbps)
{
    kbps *= 64;
    return dc_fixpt_from_fraction((int64_t)kbps, 54 * 8 * 1000);
}

/* get_pbn_from_timing (8b/10b) */
static rd_fixpt get_pbn_from_timing(uint32_t stream_kbps)
{
    return get_pbn_from_bw_in_kbps(stream_kbps);
}

/* log_vcp_x_y */
static void log_vcp_x_y(rdna3_dp_link *l, rd_fixpt avg)
{
    const uint32_t VCP_Y_PRECISION = 1000;
    int64_t vcp_x, vcp_y;
    avg = dc_fixpt_add(avg, dc_fixpt_from_fraction(1, 2 * VCP_Y_PRECISION));
    vcp_x = dc_fixpt_floor(avg);
    vcp_y = dc_fixpt_floor(dc_fixpt_mul_int(dc_fixpt_sub_int(avg, dc_fixpt_floor(avg)), VCP_Y_PRECISION));
    RLOG(l->d, "dp: MST Update Payload: set_throttled_vcp_size slot X.Y for MST stream X: %lld Y: %lld/%u",
         (long long)vcp_x, (long long)vcp_y, VCP_Y_PRECISION);
}

/* enc1_stream_encoder_set_throttled_vcp_size (set_dio_throttled_vcp_size) */
static void enc1_stream_encoder_set_throttled_vcp_size(rdna3_dp_link *l, rd_fixpt avg_time_slots_per_mtp)
{
    uint32_t x = (uint32_t)dc_fixpt_floor(avg_time_slots_per_mtp), retries;
    uint32_t y = (uint32_t)dc_fixpt_ceil(dc_fixpt_shl(dc_fixpt_sub_int(avg_time_slots_per_mtp, x), 26));
    /* y rounding up to an integer carries into x */
    if (y >> 26) {
        x += 1;
        y = 0;
    }
    WI(l->d, regDP0_DP_MSE_RATE_CNTL, l->fe,
       FIELD(DP0_DP_MSE_RATE_CNTL, DP_MSE_RATE_X, x) | FIELD(DP0_DP_MSE_RATE_CNTL, DP_MSE_RATE_Y, y));
    l->mst_vcp_x = x;
    l->mst_vcp_y = y;
    /* wait for the update on the link (DP_MSE_RATE_UPDATE_PENDING 0) */
    for (retries = 0; retries < DP_MST_UPDATE_MAX_RETRY; retries++) {
        if (!GET(RI(l->d, regDP0_DP_MSE_RATE_UPDATE, l->fe), DP0_DP_MSE_RATE_UPDATE, DP_MSE_RATE_UPDATE_PENDING))
            return;
        l->d->os.udelay(l->d->os.ctx, 10);
    }
    RERR(l->d, "dp: DIG %u: DP_MSE_RATE_UPDATE_PENDING stays set", l->fe);
}

/* fill_stream_allocation_row_info */
static void fill_stream_allocation_row_info(const rdna3_mst_alloc_table *t, uint32_t row, uint32_t *src,
                                            uint32_t *slots)
{
    if (row < t->stream_count && t->a[row].fe >= 0) {
        *src = (uint32_t)t->a[row].fe;
        *slots = t->a[row].slot_count;
    } else {
        *src = 0;
        *slots = 0;
    }
}

/* dcn10_link_encoder_update_mst_stream_allocation_table: the VC payload
 * table on the TX side, then the ACT: DP_MSE_SAT_UPDATE 1 (update with
 * trigger), wait for it to clear and the 16 MTP keepout to end */
static void dcn10_link_encoder_update_mst_stream_allocation_table(rdna3_dp_link *l, const rdna3_mst_alloc_table *t)
{
    uint32_t src, slots, v, retries = 0;
    v = RI(l->d, regDP0_DP_MSE_SAT0, l->be);
    fill_stream_allocation_row_info(t, 0, &src, &slots);
    v = (v & ~(uint32_t)(DP0_DP_MSE_SAT0__DP_MSE_SAT_SRC0_MASK | DP0_DP_MSE_SAT0__DP_MSE_SAT_SLOT_COUNT0_MASK)) |
        FIELD(DP0_DP_MSE_SAT0, DP_MSE_SAT_SRC0, src) | FIELD(DP0_DP_MSE_SAT0, DP_MSE_SAT_SLOT_COUNT0, slots);
    WI(l->d, regDP0_DP_MSE_SAT0, l->be, v);
    fill_stream_allocation_row_info(t, 1, &src, &slots);
    v = (v & ~(uint32_t)(DP0_DP_MSE_SAT0__DP_MSE_SAT_SRC1_MASK | DP0_DP_MSE_SAT0__DP_MSE_SAT_SLOT_COUNT1_MASK)) |
        FIELD(DP0_DP_MSE_SAT0, DP_MSE_SAT_SRC1, src) | FIELD(DP0_DP_MSE_SAT0, DP_MSE_SAT_SLOT_COUNT1, slots);
    WI(l->d, regDP0_DP_MSE_SAT0, l->be, v);
    v = RI(l->d, regDP0_DP_MSE_SAT1, l->be);
    fill_stream_allocation_row_info(t, 2, &src, &slots);
    v = (v & ~(uint32_t)(DP0_DP_MSE_SAT1__DP_MSE_SAT_SRC2_MASK | DP0_DP_MSE_SAT1__DP_MSE_SAT_SLOT_COUNT2_MASK)) |
        FIELD(DP0_DP_MSE_SAT1, DP_MSE_SAT_SRC2, src) | FIELD(DP0_DP_MSE_SAT1, DP_MSE_SAT_SLOT_COUNT2, slots);
    WI(l->d, regDP0_DP_MSE_SAT1, l->be, v);
    fill_stream_allocation_row_info(t, 3, &src, &slots);
    v = (v & ~(uint32_t)(DP0_DP_MSE_SAT1__DP_MSE_SAT_SRC3_MASK | DP0_DP_MSE_SAT1__DP_MSE_SAT_SLOT_COUNT3_MASK)) |
        FIELD(DP0_DP_MSE_SAT1, DP_MSE_SAT_SRC3, src) | FIELD(DP0_DP_MSE_SAT1, DP_MSE_SAT_SLOT_COUNT3, slots);
    WI(l->d, regDP0_DP_MSE_SAT1, l->be, v);
    /* 1: update the SAT with the allocation change trigger */
    v = RI(l->d, regDP0_DP_MSE_SAT_UPDATE, l->be);
    WI(l->d, regDP0_DP_MSE_SAT_UPDATE, l->be,
       (v & ~(uint32_t)DP0_DP_MSE_SAT_UPDATE__DP_MSE_SAT_UPDATE_MASK) | FIELD(DP0_DP_MSE_SAT_UPDATE, DP_MSE_SAT_UPDATE, 1));
    do {
        l->d->os.udelay(l->d->os.ctx, 10);
        v = RI(l->d, regDP0_DP_MSE_SAT_UPDATE, l->be);
        if (!GET(v, DP0_DP_MSE_SAT_UPDATE, DP_MSE_SAT_UPDATE) && !GET(v, DP0_DP_MSE_SAT_UPDATE, DP_MSE_16_MTP_KEEPOUT))
            break;
        ++retries;
    } while (retries < DP_MST_UPDATE_MAX_RETRY);
}

/* update_dio_stream_allocation_table */
static void update_dio_stream_allocation_table(rdna3_dp_link *l)
{
    dcn10_link_encoder_update_mst_stream_allocation_table(l, &l->mst_alloc);
}

/* fill_dc_mst_payload_table_from_drm */
static void fill_dc_mst_payload_table_from_drm(const rdna3_dp_link *l, int enable, uint32_t vcpi, uint32_t time_slots,
                                               rdna3_mst_alloc_table *table)
{
    rdna3_mst_alloc_table copy_of_link_table = l->mst_alloc, new_table;
    uint32_t i, current_hw_table_stream_cnt = copy_of_link_table.stream_count;
    for (i = 0; i < sizeof(new_table); i++)
        ((uint8_t *)&new_table)[i] = 0;
    if (enable) {
        if (current_hw_table_stream_cnt < RD_MST_MAX_STREAMS) {
            copy_of_link_table.a[current_hw_table_stream_cnt].vcp_id = vcpi;
            copy_of_link_table.a[current_hw_table_stream_cnt].slot_count = time_slots;
        }
    } else {
        for (i = 0; i < copy_of_link_table.stream_count; i++)
            if (copy_of_link_table.a[i].vcp_id == vcpi) {
                copy_of_link_table.a[i].vcp_id = 0;
                copy_of_link_table.a[i].slot_count = 0;
                break;
            }
    }
    for (i = 0; i < RD_MST_MAX_STREAMS; i++)
        if (copy_of_link_table.a[i].vcp_id > 0 && copy_of_link_table.a[i].slot_count > 0) {
            new_table.a[new_table.stream_count].slot_count = copy_of_link_table.a[i].slot_count;
            new_table.a[new_table.stream_count].vcp_id = copy_of_link_table.a[i].vcp_id;
            new_table.a[new_table.stream_count].fe = -1;
            new_table.stream_count++;
        }
    *table = new_table;
}

/* dm_helpers_construct_old_payload: the time slots the payload holds (up to
 * the next payload's start) */
static int dm_helpers_construct_old_payload(const rdna3_mst_mgr *mgr, int p)
{
    int next_payload_vc_start = mgr->next_start_slot, payload_vc_start = mgr->port[p].vc_start_slot, i;
    for (i = 0; i < RD_MST_MAX_PORTS; i++)
        if (i != p && mgr->port[i].used && mgr->port[i].vcpi && mgr->port[i].vc_start_slot > payload_vc_start &&
            mgr->port[i].vc_start_slot < next_payload_vc_start)
            next_payload_vc_start = mgr->port[i].vc_start_slot;
    return next_payload_vc_start - payload_vc_start;
}

/* dm_helpers_dp_mst_write_payload_allocation_table */
static int dm_helpers_dp_mst_write_payload_allocation_table(rdna3_dp_link *l, rdna3_mst_alloc_table *proposed,
                                                            int enable)
{
    rdna3_mst_mgr *mgr = l->mgr;
    rdna3_mst_port *port;
    uint32_t vcpi, time_slots;
    if (!mgr || l->mst_port < 0)
        return 0;
    port = &mgr->port[l->mst_port];
    vcpi = (uint32_t)port->vcpi;
    if (enable) {
        time_slots = (uint32_t)port->time_slots;
        /* it is OK for this to fail */
        dp_mst_add_payload_part1(mgr, l->mst_port);
    } else {
        time_slots = (uint32_t)dm_helpers_construct_old_payload(mgr, l->mst_port);
        dp_mst_remove_payload_part1(mgr, l->mst_port);
    }
    /* the DRM allocation copied to DC: slots 1-63 in DRM's order */
    fill_dc_mst_payload_table_from_drm(l, enable, vcpi, time_slots, proposed);
    return 1;
}

/* dm_helpers_dp_mst_poll_for_allocation_change_trigger */
static int dm_helpers_dp_mst_poll_for_allocation_change_trigger(rdna3_dp_link *l)
{
    if (!l->mgr || !l->mgr->mst_state)
        return ACT_FAILED;
    return dp_mst_check_act_status(l->mgr) ? ACT_FAILED : ACT_SUCCESS;
}

/* dm_helpers_dp_mst_send_payload_allocation */
static void dm_helpers_dp_mst_send_payload_allocation(rdna3_dp_link *l)
{
    if (l->mgr && dp_mst_add_payload_part2(l->mgr, l->mst_port))
        RERR(l->d, "dp: MST: ALLOCATE_PAYLOAD for port %u failed", l->mgr->port[l->mst_port].port_num);
}

/* dm_helpers_dp_mst_update_mst_mgr_for_deallocation */
static void dm_helpers_dp_mst_update_mst_mgr_for_deallocation(rdna3_dp_link *l)
{
    int old;
    if (!l->mgr || l->mst_port < 0)
        return;
    old = dm_helpers_construct_old_payload(l->mgr, l->mst_port);
    dp_mst_remove_payload_part2(l->mgr, l->mst_port, old);
}

/* update_mst_stream_alloc_table: the proposed table with the stream
 * encoders the link already had, the new VCP on 'fe' */
static void update_mst_stream_alloc_table(rdna3_dp_link *l, int fe, const rdna3_mst_alloc_table *proposed)
{
    rdna3_mst_alloc_table work;
    uint32_t i, j;
    for (i = 0; i < sizeof(work); i++)
        ((uint8_t *)&work)[i] = 0;
    for (i = 0; i < RD_MST_MAX_STREAMS; i++)
        work.a[i].fe = -1;
    if (proposed->stream_count > l->mst_alloc.stream_count + 1)
        RERR(l->d, "dp: MST: more than one new payload proposed");
    for (i = 0; i < proposed->stream_count && i < RD_MST_MAX_STREAMS; i++) {
        for (j = 0; j < l->mst_alloc.stream_count; j++)
            if (l->mst_alloc.a[j].vcp_id == proposed->a[i].vcp_id) {
                work.a[i] = l->mst_alloc.a[j];
                work.a[i].slot_count = proposed->a[i].slot_count;
                break;
            }
        if (j == l->mst_alloc.stream_count) {   /* a new VCP id */
            work.a[i].vcp_id = proposed->a[i].vcp_id;
            work.a[i].slot_count = proposed->a[i].slot_count;
            work.a[i].fe = fe;
        }
    }
    work.stream_count = proposed->stream_count;
    l->mst_alloc = work;
}

/* remove_stream_from_alloc_table */
static void remove_stream_from_alloc_table(rdna3_dp_link *l, int fe)
{
    rdna3_mst_alloc_table *t = &l->mst_alloc;
    uint32_t i = 0;
    for (; i < t->stream_count; i++)
        if (t->a[i].fe == fe)
            break;
    if (i < t->stream_count) {
        for (i++; i < t->stream_count; i++)
            t->a[i - 1] = t->a[i];
        t->a[t->stream_count - 1].fe = -1;
        t->a[t->stream_count - 1].vcp_id = 0;
        t->a[t->stream_count - 1].slot_count = 0;
        t->stream_count--;
    }
}

int dcn32_dp_mst_allocate_payload(rdna3_dp_link *l, const rdna3_mode *m, uint32_t stream_kbps)
{
    rdna3_mst_alloc_table proposed;
    rd_fixpt avg_time_slots_per_mtp, pbn, pbn_per_slot;
    int ret, slots;

    if (!l->mgr || l->mst_port < 0)
        return RDNA3_EINVAL;
    /* dm_encoder_helper_atomic_check: PBN and time slots of the stream */
    l->mgr->pbn_div = dcn32_dp_mst_pbn_divider(l);
    l->mst_pbn = (uint32_t)drm_dp_calc_pbn_mode((int)m->pixel_khz, 24 << 4);
    slots = dp_mst_find_time_slots(l->mgr, l->mst_port, (int)l->mst_pbn);
    if (slots < 0) {
        RERR(l->d, "dp: MST: no time slots for PBN %u (PBN / slot %u)", l->mst_pbn, l->mgr->pbn_div);
        return RDNA3_EINVAL;
    }
    l->mst_slots = (uint32_t)slots;
    /* the VC payload of the stream: proposed table */
    if (!dm_helpers_dp_mst_write_payload_allocation_table(l, &proposed, 1)) {
        RERR(l->d, "dp: MST: failed to update the allocation table");
        return RDNA3_EIO;
    }
    update_mst_stream_alloc_table(l, (int)l->fe, &proposed);
    RLOG(l->d, "dp: MST allocate: %u stream(s): VCP %u, %u slots (PBN %u, %u / slot) on DIG %u", l->mst_alloc.stream_count,
         l->mst_alloc.a[0].vcp_id, l->mst_alloc.a[0].slot_count, l->mst_pbn, l->mgr->pbn_div, l->fe);
    if (link_is_unknown_encoding(&l->cur_link_settings))
        return RDNA3_EIO;   /* unknown encoding format */
    /* the DP source TX for the payload */
    update_dio_stream_allocation_table(l);
    /* the down message */
    ret = dm_helpers_dp_mst_poll_for_allocation_change_trigger(l);
    l->mst_act = ret == ACT_SUCCESS;
    if (ret != ACT_LINK_LOST)
        dm_helpers_dp_mst_send_payload_allocation(l);
    /* slot X.Y for this stream only */
    pbn_per_slot = get_pbn_per_slot(l);
    if (pbn_per_slot.value == 0) {
        RERR(l->d, "dp: MST: pbn_per_slot 0");
        return RDNA3_EINVAL;
    }
    pbn = get_pbn_from_timing(stream_kbps);
    avg_time_slots_per_mtp = dc_fixpt_div(pbn, pbn_per_slot);
    log_vcp_x_y(l, avg_time_slots_per_mtp);
    enc1_stream_encoder_set_throttled_vcp_size(l, avg_time_slots_per_mtp);
    return ret == ACT_SUCCESS ? RDNA3_OK : RDNA3_EIO;
}

int dcn32_dp_mst_deallocate_payload(rdna3_dp_link *l)
{
    rdna3_mst_alloc_table proposed;
    int mst_mode = l->mgr && l->mgr->mst_state;
    /* slot X.Y 0 */
    enc1_stream_encoder_set_throttled_vcp_size(l, dc_fixpt_from_int(0));
    if (mst_mode) {
        /* the MST manager removes the payload */
        if (dm_helpers_dp_mst_write_payload_allocation_table(l, &proposed, 0))
            update_mst_stream_alloc_table(l, (int)l->fe, &proposed);
        else
            RERR(l->d, "dp: MST: failed to update the allocation table");
    } else {
        /* the link is out of MST mode (hub unplugged): DC's own removal */
        remove_stream_from_alloc_table(l, (int)l->fe);
    }
    RLOG(l->d, "dp: MST deallocate: %u stream(s) left", l->mst_alloc.stream_count);
    if (link_is_unknown_encoding(&l->cur_link_settings))
        return RDNA3_EIO;
    update_dio_stream_allocation_table(l);
    if (mst_mode)
        dm_helpers_dp_mst_poll_for_allocation_change_trigger(l);
    dm_helpers_dp_mst_update_mst_mgr_for_deallocation(l);
    return RDNA3_OK;
}
