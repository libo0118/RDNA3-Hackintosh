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
    l->mst_pbn = l->mst_req_pbn ? l->mst_req_pbn : (uint32_t)drm_dp_calc_pbn_mode((int)m->pixel_khz, 24 << 4);
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

/* ---- DSC over MST (amdgpu_dm_mst_types.c, amdgpu_dm_helpers.c) ---- */

#define DP_BRANCH_DEVICE_ID_0060AD      0x0060ad
#define DP_BRANCH_DEVICE_ID_00E04C      0x00e04c
#define DP_BRANCH_DEVICE_ID_90CC24      0x90cc24
#define DPCD_REV_12                     0x12
#define DPCD_REV_14                     0x14
#define SYNAPTICS_RC_COMMAND            0x4b2
#define SYNAPTICS_RC_RESULT             0x4b3
#define SYNAPTICS_RC_LENGTH             0x4b8
#define SYNAPTICS_RC_OFFSET             0x4bc
#define SYNAPTICS_RC_DATA               0x4c0
#define DP_BRANCH_VENDOR_SPECIFIC_START 0x50c
#define IS_SYNAPTICS_PANAMERA(name)     ((((int)(name)[4] & 0xf0) == 0x50) ? 1 : 0)
#define SYNAPTICS_CASCADED_HUB_ID       0x5a
#define IS_SYNAPTICS_CASCADED_PANAMERA(name, data) \
    ((IS_SYNAPTICS_PANAMERA(name) && ((int)(data)[2] == SYNAPTICS_CASCADED_HUB_ID)) ? 1 : 0)
#define PBN_FEC_OVERHEAD_MULTIPLIER_8B_10B    1031
#define PBN_FEC_OVERHEAD_MULTIPLIER_128B_132B 1000
/* dc_dp_types.h: the data efficiency of the 8b/10b (with FEC) and
 * 128b/132b channel codings */
#define DATA_EFFICIENCY_8b_10b_x10000              8000
#define DATA_EFFICIENCY_8b_10b_FEC_EFFICIENCY_x100 97
#define DATA_EFFICIENCY_128b_132b_x10000           9641
#define BITS_PER_DP_BYTE                           10
/* DP_LINK_BW_SET of a 128b/132b link */
#define DP_LINK_BW_10   0x01
#define DP_LINK_BW_13_5 0x04
#define DP_LINK_BW_20   0x02

static void zero(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--)
        *b++ = 0;
}

static int bytes_eq(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    while (n--)
        if (*x++ != *y++)
            return 0;
    return 1;
}

int dcn32_dp_mst_needs_dsc_aux_workaround(const rdna3_dp_link *link)
{
    return link->dpcd_caps.branch_dev_id == DP_BRANCH_DEVICE_ID_90CC24 &&
           (link->dpcd_caps.dpcd_rev == DPCD_REV_14 || link->dpcd_caps.dpcd_rev == DPCD_REV_12) &&
           (link->dpcd_caps.sink_count & 0x3f) >= 2;
}

/* is_synaptics_cascaded_panamera: the vendor data 0x50c..0x50f of the
 * first branch */
static int is_synaptics_cascaded_panamera(rdna3_dp_link *link, rdna3_mst_mgr *mgr)
{
    uint8_t branch_vendor_data[4] = { 0, 0, 0, 0 };
    if (dp_mst_aux_read(mgr, RD_MST_AUX_ROOT, DP_BRANCH_VENDOR_SPECIFIC_START, branch_vendor_data, 4) == 4) {
        if (link->dpcd_caps.branch_dev_id == DP_BRANCH_DEVICE_ID_90CC24 &&
            IS_SYNAPTICS_CASCADED_PANAMERA(link->dpcd_caps.branch_dev_name, branch_vendor_data)) {
            RLOG(link->d, "dp: MST: Synaptics Cascaded MST hub");
            return 1;
        }
    }
    return 0;
}

/* validate_dsc_caps_on_connector: the aux that decodes DSC for the port
 * (drm_dp_mst_dsc_aux_for_port, the dock's own for a DSC dock in front of
 * a display without DSC, the first branch's on a cascaded Synaptics hub),
 * then the sink's DSC caps through it */
static int validate_dsc_caps_on_connector(rdna3_dp_link *link, rdna3_mst_connector *aconnector)
{
    rdna3_mst_mgr *mgr = link->mgr;
    int port = aconnector->port, parent;
    uint8_t dsc_caps[16], dsc_branch_dec_caps_raw[3];   /* DSC branch decoder caps 0xa0 ~ 0xa2 */
    const uint8_t *dsc_branch_dec_caps = 0;
    zero(dsc_caps, sizeof(dsc_caps));
    zero(dsc_branch_dec_caps_raw, sizeof(dsc_branch_dec_caps_raw));
    aconnector->dsc_aux = dp_mst_dsc_aux_for_port(mgr, port, &aconnector->passthrough_aux);
    /* drm_dp_mst_dsc_aux_for_port only checks the port's own DSC / FEC:
     * a DSC capable dock in front of a display without them gets the
     * dock's aux */
    parent = mgr->port[port].parent;
    if (aconnector->dsc_aux == RD_MST_AUX_NONE && parent >= 0 && parent < RD_MST_MAX_BRANCHES &&
        mgr->mstb[parent].port_parent < 0 && dcn32_dp_mst_needs_dsc_aux_workaround(link))
        aconnector->dsc_aux = RD_MST_AUX_ROOT;
    /* synaptics cascaded MST hub case */
    if (is_synaptics_cascaded_panamera(link, mgr))
        aconnector->dsc_aux = RD_MST_AUX_ROOT;
    if (aconnector->dsc_aux == RD_MST_AUX_NONE)
        return 0;
    if (dp_mst_aux_read(mgr, aconnector->dsc_aux, RD_DPCD_DSC_SUPPORT, dsc_caps, 16) < 0)
        return 0;
    if (dp_mst_aux_read(mgr, aconnector->dsc_aux, RD_DPCD_DSC_BRANCH_OVERALL_THROUGHPUT_0, dsc_branch_dec_caps_raw, 3) ==
        3)
        dsc_branch_dec_caps = dsc_branch_dec_caps_raw;
    return dcn32_dsc_parse_dsc_dpcd(dsc_caps, dsc_branch_dec_caps, &aconnector->dsc_caps);
}

/* retrieve_downstream_port_device: DP_DOWNSTREAMPORT_PRESENT of the DFP
 * through the DSC aux */
static int retrieve_downstream_port_device(rdna3_dp_link *link, rdna3_mst_connector *aconnector)
{
    uint8_t ds_port_present = 0;
    if (aconnector->dsc_aux == RD_MST_AUX_NONE)
        return 0;
    if (dp_mst_aux_read(link->mgr, aconnector->dsc_aux, RD_DPCD_DOWNSTREAMPORT_PRESENT, &ds_port_present, 1) < 0) {
        RLOG(link->d, "dp: MST: failed to read downstream_port_present 0x05 from DFP of branch device");
        return 0;
    }
    aconnector->mst_downstream_port_present = ds_port_present;
    RLOG(link->d, "dp: MST: downstream port present %u, type %u", ds_port_present & 1u, (ds_port_present >> 1) & 3u);
    return 1;
}

void dcn32_dp_mst_detect_dsc(rdna3_dp_link *l, rdna3_mst_connector *aconnector, int port)
{
    zero(aconnector, sizeof(*aconnector));
    aconnector->valid = 1;
    aconnector->port = port;
    aconnector->dsc_aux = aconnector->passthrough_aux = RD_MST_AUX_NONE;
    if (!l->mgr || port < 0 || port >= RD_MST_MAX_PORTS || !l->mgr->port[port].used)
        return;
    /* dm_dp_mst_get_modes */
    if (!validate_dsc_caps_on_connector(l, aconnector))
        zero(&aconnector->dsc_caps, sizeof(aconnector->dsc_caps));
    if (!retrieve_downstream_port_device(l, aconnector))
        aconnector->mst_downstream_port_present = 0;
    RLOG(l->d, "dp: MST port %u: DSC aux %d, passthrough aux %d, DSC %s", l->mgr->port[port].port_num,
         aconnector->dsc_aux, aconnector->passthrough_aux, aconnector->dsc_caps.is_dsc_supported ? "yes" : "no");
}

/* get_fec_overhead_multiplier */
static uint16_t get_fec_overhead_multiplier(const rdna3_dp_link *dc_link)
{
    uint16_t fec_overhead_multiplier_x1000 = PBN_FEC_OVERHEAD_MULTIPLIER_8B_10B;
    if (dcn32_dp_mst_decide_link_encoding_format(dc_link) == RD_DP_128b_132b_ENCODING)
        fec_overhead_multiplier_x1000 = PBN_FEC_OVERHEAD_MULTIPLIER_128B_132B;
    return fec_overhead_multiplier_x1000;
}

/* kbps_to_peak_pbn */
static int kbps_to_peak_pbn(int kbps, uint16_t fec_overhead_multiplier_x1000)
{
    uint64_t peak_kbps = (uint64_t)(uint32_t)kbps;
    peak_kbps *= 1006;
    peak_kbps *= fec_overhead_multiplier_x1000;
    peak_kbps /= 1000 * 1000;
    return (int)((peak_kbps * 64 + (54 * 8 * 1000) - 1) / (54 * 8 * 1000));
}

int dcn32_dp_mst_kbps_to_peak_pbn(const rdna3_dp_link *l, uint32_t kbps)
{
    return kbps_to_peak_pbn((int)kbps, get_fec_overhead_multiplier(l));
}

/* struct dsc_mst_fairness_params */
typedef struct dsc_mst_fairness_params {
    rdna3_dsc_timing *timing;
    const rdna3_dsc_dec_caps *sink_caps;      /* sink->dsc_caps.dsc_dec_caps */
    rdna3_dsc_bw_range bw_range;
    int compression_possible;
    int port;
    int clock_force_enable;
    uint32_t num_slices_h, num_slices_v, bpp_overwrite;
    const rdna3_mst_connector *aconnector;
    rdna3_mst_stream *stream;                 /* timing.dsc_cfg.mst_pbn */
} dsc_mst_fairness_params;

/* the default DSC options with the display's max bits per pixel */
static void dsc_options_for(const rdna3_mst_connector *aconnector, rdna3_dsc_options *o)
{
    dcn32_dsc_default_options(o);
    o->max_target_bpp_limit_override_x16 = aconnector->max_dsc_bpp * 16;
}

/* set_dsc_configs_from_fairness_vars */
static void set_dsc_configs_from_fairness_vars(dsc_mst_fairness_params *params, rdna3_mst_fairness_vars *vars,
                                               int count, int k)
{
    rdna3_dsc_options dsc_options;
    int i;
    for (i = 0; i < count; i++) {
        dsc_options_for(params[i].aconnector, &dsc_options);
        zero(&params[i].timing->dsc_cfg, sizeof(params[i].timing->dsc_cfg));
        params[i].timing->dsc = 0;
        if (vars[i + k].dsc_enabled &&
            dcn32_dsc_compute_config(params[i].sink_caps, &dsc_options, 0, params[i].timing, &params[i].timing->dsc_cfg)) {
            params[i].timing->dsc = 1;
            if (params[i].bpp_overwrite)
                params[i].timing->dsc_cfg.bits_per_pixel = params[i].bpp_overwrite;
            else
                params[i].timing->dsc_cfg.bits_per_pixel = (uint32_t)vars[i + k].bpp_x16;
            if (params[i].num_slices_h)
                params[i].timing->dsc_cfg.num_slices_h = params[i].num_slices_h;
            if (params[i].num_slices_v)
                params[i].timing->dsc_cfg.num_slices_v = params[i].num_slices_v;
        }
        params[i].stream->mst_pbn = (uint32_t)vars[i + k].pbn;
    }
}

/* bpp_x16_from_pbn */
static int bpp_x16_from_pbn(const dsc_mst_fairness_params *param, int pbn)
{
    rdna3_dsc_config dsc_config;
    rdna3_dsc_options dsc_options;
    uint64_t kbps;
    zero(&dsc_config, sizeof(dsc_config));
    dsc_options_for(param->aconnector, &dsc_options);
    kbps = (uint64_t)(uint32_t)pbn * 994 * 8 * 54 / 64;
    dcn32_dsc_compute_config(param->sink_caps, &dsc_options, (uint32_t)kbps, param->timing, &dsc_config);
    return (int)dsc_config.bits_per_pixel;
}

/* drm_atomic_get_mst_topology_state: the manager's payloads, the PBN
 * divider dm_encoder_helper_atomic_check gives it */
static rdna3_mst_atomic *get_mst_topology_state(rdna3_mst_atomic_state *state, const rdna3_dp_link *link)
{
    uint32_t i;
    for (i = 0; i < state->ntopo; i++)
        if (state->topo[i].mgr == link->mgr)
            return &state->topo[i].st;
    if (state->ntopo >= RD_MST_MAX_TOPO || !link->mgr)
        return 0;
    state->topo[i].mgr = link->mgr;
    dp_mst_atomic_init(link->mgr, &state->topo[i].st);
    if (!state->topo[i].st.pbn_div)
        state->topo[i].st.pbn_div = dcn32_dp_mst_pbn_divider(link);
    if (!state->topo[i].st.total_avail_slots)
        state->topo[i].st.total_avail_slots = 63;   /* 8b/10b: slot 0 is the MTP header */
    state->ntopo++;
    return &state->topo[i].st;
}

/* drm_dp_atomic_find_time_slots on the stream's topology state */
static int find_time_slots(rdna3_mst_atomic_state *state, const dsc_mst_fairness_params *param, int pbn)
{
    rdna3_mst_atomic *st = get_mst_topology_state(state, param->stream->link);
    if (!st)
        return RDNA3_EINVAL;
    return dp_mst_atomic_find_time_slots(param->stream->link->mgr, st, param->port, pbn);
}

int dcn32_dp_mst_atomic_check(const rdna3_mst_atomic_state *state)
{
    uint32_t i;
    int ret;
    for (i = 0; i < state->ntopo; i++) {
        ret = dp_mst_atomic_check(state->topo[i].mgr, &state->topo[i].st);
        if (ret)
            return ret;
    }
    return RDNA3_OK;
}

/* increase_dsc_bpp: the free time slots shared out, the stream with the
 * least slack to its uncompressed rate first. (Linux indexes vars with
 * next_index alone here, which is the stream's only for the first link:
 * k is added, as everywhere else.) */
static int increase_dsc_bpp(rdna3_mst_atomic_state *state, rdna3_mst_atomic *mst_state, const rdna3_dp_link *dc_link,
                            dsc_mst_fairness_params *params, rdna3_mst_fairness_vars *vars, int count, int k)
{
    int i, bpp_increased[RD_DCN_MAX_PIPES], initial_slack[RD_DCN_MAX_PIPES];
    int min_initial_slack, next_index, remaining_to_increase = 0, link_timeslots_used, fair_pbn_alloc, ret;
    int pbn_div = (int)mst_state->pbn_div;
    uint16_t fec_overhead_multiplier_x1000 = get_fec_overhead_multiplier(dc_link);

    for (i = 0; i < count; i++) {
        if (vars[i + k].dsc_enabled) {
            initial_slack[i] =
                kbps_to_peak_pbn((int)params[i].bw_range.max_kbps, fec_overhead_multiplier_x1000) - vars[i + k].pbn;
            bpp_increased[i] = 0;
            remaining_to_increase += 1;
        } else {
            initial_slack[i] = 0;
            bpp_increased[i] = 1;
        }
    }
    if (!pbn_div)
        return RDNA3_EINVAL;
    while (remaining_to_increase) {
        rdna3_mst_fairness_vars *v;
        next_index = -1;
        min_initial_slack = -1;
        for (i = 0; i < count; i++)
            if (!bpp_increased[i] && (min_initial_slack == -1 || min_initial_slack > initial_slack[i])) {
                min_initial_slack = initial_slack[i];
                next_index = i;
            }
        if (next_index == -1)
            break;
        v = &vars[next_index + k];
        link_timeslots_used = 0;
        for (i = 0; i < count; i++)
            link_timeslots_used += (vars[i + k].pbn + pbn_div - 1) / pbn_div;
        fair_pbn_alloc = (63 - link_timeslots_used) / remaining_to_increase * pbn_div;
        if (initial_slack[next_index] > fair_pbn_alloc) {
            v->pbn += fair_pbn_alloc;
            ret = find_time_slots(state, &params[next_index], v->pbn);
            if (ret < 0)
                return ret;
            ret = dcn32_dp_mst_atomic_check(state);
            if (ret == 0) {
                v->bpp_x16 = bpp_x16_from_pbn(&params[next_index], v->pbn);
            } else {
                v->pbn -= fair_pbn_alloc;
                ret = find_time_slots(state, &params[next_index], v->pbn);
                if (ret < 0)
                    return ret;
            }
        } else {
            v->pbn += initial_slack[next_index];
            ret = find_time_slots(state, &params[next_index], v->pbn);
            if (ret < 0)
                return ret;
            ret = dcn32_dp_mst_atomic_check(state);
            if (ret == 0) {
                v->bpp_x16 = (int)params[next_index].bw_range.max_target_bpp_x16;
            } else {
                v->pbn -= initial_slack[next_index];
                ret = find_time_slots(state, &params[next_index], v->pbn);
                if (ret < 0)
                    return ret;
            }
        }
        bpp_increased[next_index] = 1;
        remaining_to_increase--;
    }
    return 0;
}

/* try_disable_dsc: the streams at their max DSC bpp tried uncompressed,
 * the largest gain first (vars indexed with k, as in increase_dsc_bpp) */
static int try_disable_dsc(rdna3_mst_atomic_state *state, const rdna3_dp_link *dc_link, dsc_mst_fairness_params *params,
                           rdna3_mst_fairness_vars *vars, int count, int k)
{
    int i, tried[RD_DCN_MAX_PIPES], kbps_increase[RD_DCN_MAX_PIPES], max_kbps_increase, next_index;
    int remaining_to_try = 0, ret, var_pbn;
    uint16_t fec_overhead_multiplier_x1000 = get_fec_overhead_multiplier(dc_link);

    for (i = 0; i < count; i++) {
        if (vars[i + k].dsc_enabled && vars[i + k].bpp_x16 == (int)params[i].bw_range.max_target_bpp_x16 &&
            params[i].clock_force_enable == RD_DSC_CLK_FORCE_DEFAULT) {
            kbps_increase[i] = (int)params[i].bw_range.stream_kbps - (int)params[i].bw_range.max_kbps;
            tried[i] = 0;
            remaining_to_try += 1;
        } else {
            kbps_increase[i] = 0;
            tried[i] = 1;
        }
    }
    while (remaining_to_try) {
        rdna3_mst_fairness_vars *v;
        next_index = -1;
        max_kbps_increase = -1;
        for (i = 0; i < count; i++)
            if (!tried[i] && (max_kbps_increase == -1 || max_kbps_increase < kbps_increase[i])) {
                max_kbps_increase = kbps_increase[i];
                next_index = i;
            }
        if (next_index == -1)
            break;
        v = &vars[next_index + k];
        var_pbn = v->pbn;
        v->pbn = kbps_to_peak_pbn((int)params[next_index].bw_range.stream_kbps, fec_overhead_multiplier_x1000);
        ret = find_time_slots(state, &params[next_index], v->pbn);
        if (ret < 0) {
            v->pbn = var_pbn;
            return ret;
        }
        ret = dcn32_dp_mst_atomic_check(state);
        if (ret == 0) {
            /* greedily disable dsc */
            v->dsc_enabled = 0;
            v->bpp_x16 = 0;
        } else {
            /* restore optimized pbn value */
            v->pbn = var_pbn;
            ret = find_time_slots(state, &params[next_index], v->pbn);
            if (ret < 0)
                return ret;
        }
        tried[next_index] = 1;
        remaining_to_try--;
    }
    return 0;
}

/* compute_mst_dsc_configs_for_link: no compression; else max compression
 * for each stream that can, then the free slots shared out
 * (increase_dsc_bpp), then compression off where it fits (try_disable_dsc) */
static int compute_mst_dsc_configs_for_link(rdna3_mst_atomic_state *state, rdna3_mst_dc_state *dc_state,
                                            const rdna3_dp_link *dc_link, rdna3_mst_fairness_vars *vars,
                                            int *link_vars_start_index)
{
    dsc_mst_fairness_params params[RD_DCN_MAX_PIPES];
    rdna3_mst_atomic *mst_state = get_mst_topology_state(state, dc_link);
    int count = 0, i, k, ret, debugfs_overwrite = 0;
    uint32_t s;
    uint16_t fec_overhead_multiplier_x1000 = get_fec_overhead_multiplier(dc_link);

    zero(params, sizeof(params));
    if (!mst_state)
        return RDNA3_EINVAL;
    /* Set up params */
    for (s = 0; s < dc_state->stream_count && count < RD_DCN_MAX_PIPES; s++) {
        rdna3_mst_stream *stream = dc_state->streams[s];
        const rdna3_mst_connector *aconnector;
        rdna3_dsc_policy dsc_policy;
        if (stream->link != dc_link)
            continue;
        aconnector = stream->aconnector;
        if (!aconnector || aconnector->port < 0)
            continue;
        stream->timing.dsc = 0;
        params[count].timing = &stream->timing;
        params[count].sink_caps = &aconnector->dsc_caps;
        params[count].aconnector = aconnector;
        params[count].stream = stream;
        params[count].port = aconnector->port;
        params[count].clock_force_enable = aconnector->dsc_force_enable;
        if (params[count].clock_force_enable == RD_DSC_CLK_FORCE_ENABLE)
            debugfs_overwrite = 1;
        params[count].num_slices_h = aconnector->dsc_num_slices_h;
        params[count].num_slices_v = aconnector->dsc_num_slices_v;
        params[count].bpp_overwrite = aconnector->dsc_bits_per_pixel;
        params[count].compression_possible = aconnector->dsc_caps.is_dsc_supported;
        zero(&dsc_policy, sizeof(dsc_policy));
        stream->timing.link_encoding = dcn32_dp_highest_encoding(dc_link);
        dcn32_dsc_get_policy(params[count].timing, 0, &dsc_policy);
        if (!dcn32_dsc_compute_bandwidth_range(dsc_policy.min_target_bpp * 16, dsc_policy.max_target_bpp * 16,
                                               &aconnector->dsc_caps, &stream->timing, &params[count].bw_range))
            params[count].bw_range.stream_kbps = dcn32_dsc_timing_kbps(&stream->timing);
        RDBG(dc_link->d, "dp: MST_DSC #%d: max_kbps = %u, min_kbps = %u, uncompressed_kbps = %u", count,
             params[count].bw_range.max_kbps, params[count].bw_range.min_kbps, params[count].bw_range.stream_kbps);
        count++;
    }
    if (count == 0)
        return 0;
    /* k is start index of vars for current phy link used by mst hub */
    k = *link_vars_start_index;
    /* set vars start index for next mst hub phy link */
    *link_vars_start_index += count;

    /* Try no compression */
    for (i = 0; i < count; i++) {
        vars[i + k].aconnector = params[i].aconnector;
        vars[i + k].pbn = kbps_to_peak_pbn((int)params[i].bw_range.stream_kbps, fec_overhead_multiplier_x1000);
        vars[i + k].dsc_enabled = 0;
        vars[i + k].bpp_x16 = 0;
        ret = find_time_slots(state, &params[i], vars[i + k].pbn);
        if (ret < 0)
            return ret;
    }
    ret = dcn32_dp_mst_atomic_check(state);
    if (ret == 0 && !debugfs_overwrite) {
        set_dsc_configs_from_fairness_vars(params, vars, count, k);
        return 0;
    } else if (ret != RDNA3_ENOSPC) {
        return ret;
    }

    /* Try max compression */
    for (i = 0; i < count; i++) {
        if (params[i].compression_possible && params[i].clock_force_enable != RD_DSC_CLK_FORCE_DISABLE) {
            vars[i + k].pbn = kbps_to_peak_pbn((int)params[i].bw_range.min_kbps, fec_overhead_multiplier_x1000);
            vars[i + k].dsc_enabled = 1;
            vars[i + k].bpp_x16 = (int)params[i].bw_range.min_target_bpp_x16;
        } else {
            vars[i + k].pbn = kbps_to_peak_pbn((int)params[i].bw_range.stream_kbps, fec_overhead_multiplier_x1000);
            vars[i + k].dsc_enabled = 0;
            vars[i + k].bpp_x16 = 0;
        }
        ret = find_time_slots(state, &params[i], vars[i + k].pbn);
        if (ret < 0)
            return ret;
    }
    ret = dcn32_dp_mst_atomic_check(state);
    if (ret != 0)
        return ret;

    /* Optimize degree of compression */
    ret = increase_dsc_bpp(state, mst_state, dc_link, params, vars, count, k);
    if (ret < 0) {
        RDBG(dc_link->d, "dp: MST_DSC: failed to optimize compression");
        return ret;
    }
    ret = try_disable_dsc(state, dc_link, params, vars, count, k);
    if (ret < 0) {
        RDBG(dc_link->d, "dp: MST_DSC: failed to disable compression");
        return ret;
    }
    set_dsc_configs_from_fairness_vars(params, vars, count, k);
    return 0;
}

/* is_dsc_need_re_compute: a stream on the hub that changes mode, or one
 * that leaves it */
static int is_dsc_need_re_compute(const rdna3_mst_atomic_state *state, const rdna3_mst_dc_state *dc_state,
                                  const rdna3_dp_link *dc_link)
{
    const rdna3_mst_connector *stream_on_link[RD_DCN_MAX_PIPES];
    int new_stream_on_link_num = 0, j;
    uint32_t i;

    /* only check phy used by dsc mst branch */
    if (!dc_link->mst)
        return 0;
    /* add a check for older MST DSC with no virtual DPCDs */
    if (dcn32_dp_mst_needs_dsc_aux_workaround(dc_link) &&
        !(dc_link->dpcd_caps.dsc_basic[0] & (RD_DP_DSC_DECOMPRESSION_IS_SUPPORTED | RD_DP_DSC_PASSTHROUGH_IS_SUPPORTED)))
        return 0;
    for (i = 0; i < RD_DCN_MAX_PIPES; i++)
        stream_on_link[i] = 0;
    /* check if there is mode change in new request */
    for (i = 0; i < dc_state->stream_count; i++) {
        const rdna3_mst_stream *stream = dc_state->streams[i];
        if (!stream || stream->link != dc_link)
            continue;
        if (!stream->aconnector || stream->aconnector->dsc_aux == RD_MST_AUX_NONE)
            continue;
        if (new_stream_on_link_num < RD_DCN_MAX_PIPES)
            stream_on_link[new_stream_on_link_num++] = stream->aconnector;
        if (stream->enable && stream->active &&
            (stream->mode_changed || stream->active_changed || stream->connectors_changed))
            return 1;
    }
    if (new_stream_on_link_num == 0)
        return 0;
    /* check current_state if there stream on link but it is not in new
     * request state */
    for (i = 0; state->current && i < state->current->stream_count; i++) {
        const rdna3_mst_stream *stream = state->current->streams[i];
        if (!stream || stream->link != dc_link || !stream->aconnector)
            continue;
        for (j = 0; j < new_stream_on_link_num; j++)
            if (stream_on_link[j] && stream->aconnector == stream_on_link[j])
                break;
        if (j == new_stream_on_link_num)
            return 1;   /* not in new state */
    }
    return 0;
}

/* dc_stream_add_dsc_to_resource: dcn20_acquire_dsc gives the pipe its own
 * DSC engine (dsc_inst = the pipe) */
static int dc_stream_add_dsc_to_resource(const rdna3_mst_stream *stream)
{
    return stream->link->dsc_inst < RD_DCN_MAX_PIPES ? RDNA3_OK : RDNA3_EINVAL;
}

int dcn32_dp_mst_compute_dsc_configs_for_state(rdna3_mst_atomic_state *state, rdna3_mst_dc_state *dc_state,
                                               rdna3_mst_fairness_vars *vars)
{
    int computed_streams[RD_DCN_MAX_PIPES], link_vars_start_index = 0, ret = 0;
    uint32_t i, j;
    for (i = 0; i < dc_state->stream_count; i++)
        computed_streams[i] = 0;
    for (i = 0; i < dc_state->stream_count; i++) {
        rdna3_mst_stream *stream = dc_state->streams[i];
        if (!stream->is_mst)
            continue;
        if (!stream->aconnector || stream->aconnector->port < 0 || !stream->link->mgr)
            continue;
        if (!stream->aconnector->dsc_caps.is_dsc_supported)
            continue;
        if (computed_streams[i])
            continue;
        /* dcn20_remove_stream_from_ctx: the DSC engine of the stream's pipe
         * is its own (nothing pooled to give back) */
        if (!is_dsc_need_re_compute(state, dc_state, stream->link))
            continue;
        ret = compute_mst_dsc_configs_for_link(state, dc_state, stream->link, vars, &link_vars_start_index);
        if (ret != 0)
            return ret;
        for (j = 0; j < dc_state->stream_count; j++)
            if (dc_state->streams[j]->link == stream->link)
                computed_streams[j] = 1;
    }
    for (i = 0; i < dc_state->stream_count; i++)
        if (dc_state->streams[i]->timing.dsc && dc_stream_add_dsc_to_resource(dc_state->streams[i]) != RDNA3_OK) {
            RDBG(dc_state->streams[i]->link->d, "dp: MST_DSC: failed to request dsc hw resource for stream %u", i);
            return RDNA3_EINVAL;
        }
    return ret;
}

/* pre_compute_mst_dsc_configs_for_state: the same without the resources */
static int pre_compute_mst_dsc_configs_for_state(rdna3_mst_atomic_state *state, rdna3_mst_dc_state *dc_state,
                                                 rdna3_mst_fairness_vars *vars)
{
    int computed_streams[RD_DCN_MAX_PIPES], link_vars_start_index = 0, ret = 0;
    uint32_t i, j;
    for (i = 0; i < dc_state->stream_count; i++)
        computed_streams[i] = 0;
    for (i = 0; i < dc_state->stream_count; i++) {
        rdna3_mst_stream *stream = dc_state->streams[i];
        if (!stream->is_mst)
            continue;
        if (!stream->aconnector || stream->aconnector->port < 0 || !stream->link->mgr)
            continue;
        if (!stream->aconnector->dsc_caps.is_dsc_supported)
            continue;
        if (computed_streams[i])
            continue;
        if (!is_dsc_need_re_compute(state, dc_state, stream->link))
            continue;
        ret = compute_mst_dsc_configs_for_link(state, dc_state, stream->link, vars, &link_vars_start_index);
        if (ret != 0)
            return ret;
        for (j = 0; j < dc_state->stream_count; j++)
            if (dc_state->streams[j]->link == stream->link)
                computed_streams[j] = 1;
    }
    return ret;
}

/* is_link_to_dschub */
static int is_link_to_dschub(const rdna3_dp_link *dc_link)
{
    /* only check phy used by dsc mst branch */
    if (!dc_link->mst)
        return 0;
    return (dc_link->dpcd_caps.dsc_basic[0] &
            (RD_DP_DSC_DECOMPRESSION_IS_SUPPORTED | RD_DP_DSC_PASSTHROUGH_IS_SUPPORTED)) != 0;
}

/* is_dsc_precompute_needed: a CRTC of the commit drives a DSC hub */
static int is_dsc_precompute_needed(const rdna3_mst_dc_state *new_state)
{
    int ret = 0;
    uint32_t i;
    for (i = 0; i < new_state->stream_count; i++) {
        const rdna3_mst_stream *stream = new_state->streams[i];
        /* amdgpu_dm_find_first_crtc_matching_connector */
        if (!stream->aconnector) {
            ret = 0;
            break;
        }
        if (stream->link && is_link_to_dschub(stream->link))
            ret = 1;
    }
    return ret;
}

/* dc_is_timing_changed */
static int dc_is_timing_changed(const rdna3_mst_stream *cur, const rdna3_mst_stream *nw)
{
    return !bytes_eq(&cur->timing, &nw->timing, sizeof(cur->timing));
}

/* find_crtc_index_in_state_by_stream: the stream of the commit that
 * replaces 'stream' (the one on its connector) */
static int find_crtc_index_in_state_by_stream(const rdna3_mst_dc_state *new_state, const rdna3_mst_stream *stream)
{
    uint32_t i;
    for (i = 0; i < new_state->stream_count; i++)
        if (new_state->streams[i]->aconnector == stream->aconnector && new_state->streams[i]->link == stream->link)
            return (int)i;
    return -1;
}

int dcn32_dp_mst_pre_validate_dsc(rdna3_mst_atomic_state *state, rdna3_mst_dc_state *new_state,
                                  rdna3_mst_fairness_vars *vars)
{
    rdna3_mst_stream local_streams[RD_DCN_MAX_PIPES];
    rdna3_mst_dc_state local_dc_state;
    const rdna3_mst_dc_state *context = state->current;
    uint32_t i;
    int ret;

    if (!is_dsc_precompute_needed(new_state))
        return 0;
    if (!context)
        return 0;
    /* the current streams, the ones the commit changes replaced by their
     * new copies (create_validate_stream_for_sink) */
    zero(&local_dc_state, sizeof(local_dc_state));
    local_dc_state.stream_count = context->stream_count;
    for (i = 0; i < context->stream_count && i < RD_DCN_MAX_PIPES; i++) {
        int ind = find_crtc_index_in_state_by_stream(new_state, context->streams[i]);
        local_streams[i] = ind >= 0 ? *new_state->streams[ind] : *context->streams[i];
        local_dc_state.streams[i] = &local_streams[i];
    }
    /* the payloads it tries are the commit's own (compute_mst_dsc_configs_for_state sets them again) */
    ret = pre_compute_mst_dsc_configs_for_state(state, &local_dc_state, vars);
    if (ret != 0) {
        RDBG(context->streams[0]->link->d, "dp: MST_DSC: pre_compute_mst_dsc_configs_for_state() failed");
        return RDNA3_EINVAL;
    }
    /* the same timing as the stream runs: no mode change */
    for (i = 0; i < local_dc_state.stream_count; i++) {
        int ind = find_crtc_index_in_state_by_stream(new_state, context->streams[i]);
        if (dc_is_timing_changed(context->streams[i], &local_streams[i]))
            RDBG(context->streams[i]->link->d, "dp: MST_DSC: crtc[%u] needs mode_change", i);
        else if (ind >= 0)
            new_state->streams[ind]->mode_changed = 0;
    }
    return 0;
}

/* dp_get_link_current_set_bw: what the last link (the port's sink) was
 * trained to */
static int dp_get_link_current_set_bw(rdna3_mst_mgr *mgr, int aux, uint32_t *cur_link_bw)
{
    uint32_t total_data_bw_efficiency_x10000 = 0, link_rate_per_lane_kbps = 0, link_rate;
    uint8_t dp_link_encoding = 0, lane_count = 0, link_bw_set = 0;

    *cur_link_bw = 0;
    if (dp_mst_aux_read(mgr, aux, RD_DPCD_MAIN_LINK_CHANNEL_CODING_SET, &dp_link_encoding, 1) != 1 ||
        dp_mst_aux_read(mgr, aux, RD_DPCD_LANE_COUNT_SET, &lane_count, 1) != 1 ||
        dp_mst_aux_read(mgr, aux, RD_DPCD_LINK_BW_SET, &link_bw_set, 1) != 1)
        return 0;
    switch (dp_link_encoding) {
    case RD_DP_8b_10b_ENCODING:
        link_rate = link_bw_set;
        link_rate_per_lane_kbps = link_rate * RD_DP_LINK_RATE_REF_FREQ_IN_KHZ * BITS_PER_DP_BYTE;
        total_data_bw_efficiency_x10000 = DATA_EFFICIENCY_8b_10b_x10000;
        total_data_bw_efficiency_x10000 /= 100;
        total_data_bw_efficiency_x10000 *= DATA_EFFICIENCY_8b_10b_FEC_EFFICIENCY_x100;
        break;
    case RD_DP_128b_132b_ENCODING:
        switch (link_bw_set) {
        case DP_LINK_BW_10:
            link_rate = RD_DP_LINK_RATE_UHBR10;
            break;
        case DP_LINK_BW_13_5:
            link_rate = RD_DP_LINK_RATE_UHBR13_5;
            break;
        case DP_LINK_BW_20:
            link_rate = RD_DP_LINK_RATE_UHBR20;
            break;
        default:
            return 0;
        }
        link_rate_per_lane_kbps = link_rate * 10000;
        total_data_bw_efficiency_x10000 = DATA_EFFICIENCY_128b_132b_x10000;
        break;
    default:
        return 0;
    }
    *cur_link_bw = link_rate_per_lane_kbps * (lane_count & 0x1fu) / 10000 * total_data_bw_efficiency_x10000;
    return 1;
}

/* is_dsc_common_config_possible: a DSC bits per pixel range the source
 * and the sink share */
static int is_dsc_common_config_possible(const rdna3_mst_connector *aconnector, const rdna3_dsc_timing *timing,
                                         rdna3_dsc_bw_range *bw_range)
{
    rdna3_dsc_policy dsc_policy;
    zero(&dsc_policy, sizeof(dsc_policy));
    dcn32_dsc_get_policy(timing, 0, &dsc_policy);
    dcn32_dsc_compute_bandwidth_range(dsc_policy.min_target_bpp * 16, dsc_policy.max_target_bpp * 16,
                                      &aconnector->dsc_caps, timing, bw_range);
    return bw_range->max_target_bpp_x16 && bw_range->min_target_bpp_x16;
}

int dcn32_dp_mst_is_port_support_mode(rdna3_dp_link *l, const rdna3_mst_connector *aconnector,
                                      rdna3_dsc_timing *timing)
{
    rdna3_mst_mgr *mgr = l->mgr;
    const rdna3_mst_port *port;
    uint32_t branch_max_throughput_mps = 0, end_to_end_bw_in_kbps, root_link_bw_in_kbps, virtual_channel_bw_in_kbps;
    uint32_t stream_kbps;
    rdna3_dsc_bw_range bw_range;
    rdna3_dsc_options dsc_options;

    if (!mgr || aconnector->port < 0 || aconnector->port >= RD_MST_MAX_PORTS || !mgr->port[aconnector->port].used)
        return RDNA3_EINVAL;
    port = &mgr->port[aconnector->port];
    zero(&bw_range, sizeof(bw_range));
    /* DSC unnecessary case: the timing within the end-to-end bandwidth */
    timing->dsc = 0;
    timing->link_encoding = dcn32_dp_highest_encoding(l);
    stream_kbps = dcn32_dsc_timing_kbps(timing);
    root_link_bw_in_kbps = dcn32_dp_link_bw_kbps(l, dcn32_dp_link_cap(l));
    virtual_channel_bw_in_kbps = kbps_from_pbn(port->full_pbn);
    /* pick the end to end bw bottleneck */
    end_to_end_bw_in_kbps = root_link_bw_in_kbps < virtual_channel_bw_in_kbps ? root_link_bw_in_kbps
                                                                               : virtual_channel_bw_in_kbps;
    if (stream_kbps <= end_to_end_bw_in_kbps)
        return RDNA3_OK;   /* no dsc required. End-to-end bw sufficient */
    /* DSC necessary case (with rdna3dsc=1, as an SST stream's) */
    if (aconnector->dsc_aux == RD_MST_AUX_NONE || !l->dsc_policy)
        return RDNA3_EINVAL;
    if (!is_dsc_common_config_possible(aconnector, timing, &bw_range)) {
        RDBG(l->d, "dp: MST_DSC require dsc but can't find common dsc config");
        return RDNA3_EINVAL;
    }
    if (aconnector->passthrough_aux != RD_MST_AUX_NONE) {
        /* capable of dsc passthough: dsc bitstream along the entire path */
        if (bw_range.min_kbps > end_to_end_bw_in_kbps) {
            RDBG(l->d, "dp: MST_DSC: max dsc compression bw can't fit into end-to-end bw");
            return RDNA3_EINVAL;
        }
    } else {
        /* dsc bitstream decoded at the dp last link */
        int immediate_upstream_port = -1;
        uint32_t end_link_bw = 0;
        /* the last DP link's bandwidth */
        if (dp_get_link_current_set_bw(mgr, aconnector->port, &end_link_bw) && stream_kbps > end_link_bw) {
            RDBG(l->d, "dp: MST_DSC: dsc decode at last link: mode can't fit into last link");
            return RDNA3_EINVAL;
        }
        /* the virtual channel between the source and the link before the last */
        if (port->parent >= 0 && port->parent < RD_MST_MAX_BRANCHES)
            immediate_upstream_port = mgr->mstb[port->parent].port_parent;
        if (immediate_upstream_port >= 0) {
            virtual_channel_bw_in_kbps = kbps_from_pbn(mgr->port[immediate_upstream_port].full_pbn);
            if (root_link_bw_in_kbps < virtual_channel_bw_in_kbps)
                virtual_channel_bw_in_kbps = root_link_bw_in_kbps;
            if (bw_range.min_kbps > virtual_channel_bw_in_kbps) {
                RDBG(l->d, "dp: MST_DSC: dsc decode at last link: max compression can't fit into MST bw");
                return RDNA3_EINVAL;
            }
        }
    }
    /* confirm the dsc config */
    dsc_options_for(aconnector, &dsc_options);
    if (!dcn32_dsc_compute_config(&aconnector->dsc_caps, &dsc_options, end_to_end_bw_in_kbps, timing,
                                  &timing->dsc_cfg)) {
        RDBG(l->d, "dp: MST_DSC require dsc but can't find appropriate dsc config");
        return RDNA3_EINVAL;
    }
    timing->dsc = 1;
    /* the branch's DSC output throughput (branch_overall_throughput_0_mps) */
    switch (timing->pixel_encoding) {
    case RD_PIXEL_ENCODING_RGB:
    case RD_PIXEL_ENCODING_YCBCR444:
        branch_max_throughput_mps = aconnector->dsc_caps.branch_overall_throughput_0_mps;
        break;
    case RD_PIXEL_ENCODING_YCBCR422:
    case RD_PIXEL_ENCODING_YCBCR420:
        branch_max_throughput_mps = aconnector->dsc_caps.branch_overall_throughput_1_mps;
        break;
    default:
        break;
    }
    if (branch_max_throughput_mps != 0 && timing->pix_clk_100hz / 10 > branch_max_throughput_mps * 1000) {
        RDBG(l->d, "dp: MST_DSC require dsc but max throughput mps fails");
        return RDNA3_EINVAL;
    }
    return RDNA3_OK;
}

/* ---- DSC on the MST sink (amdgpu_dm_helpers.c) ---- */

/* execute_synaptics_rc_command: a remote control command of the Synaptics
 * hub through its vendor DPCD */
static int execute_synaptics_rc_command(rdna3_dp_link *l, int aux, int is_write_cmd, uint8_t cmd, uint32_t length,
                                        uint32_t offset, uint8_t *data)
{
    uint8_t rc_data[16], rc_offset[4], rc_length[2], rc_cmd, rc_result = 0xff, i;
    int ret = 0, success;
    zero(rc_data, sizeof(rc_data));
    if (is_write_cmd) {
        /* write rc data */
        for (i = 0; i < length && i < sizeof(rc_data); i++)
            rc_data[i] = data[i];
        ret = dp_mst_aux_write(l->mgr, aux, SYNAPTICS_RC_DATA, rc_data, sizeof(rc_data));
    }
    /* write rc offset */
    rc_offset[0] = (uint8_t)(offset & 0xff);
    rc_offset[1] = (uint8_t)((offset >> 8) & 0xff);
    rc_offset[2] = (uint8_t)((offset >> 16) & 0xff);
    rc_offset[3] = (uint8_t)((offset >> 24) & 0xff);
    ret = dp_mst_aux_write(l->mgr, aux, SYNAPTICS_RC_OFFSET, rc_offset, sizeof(rc_offset));
    /* write rc length */
    rc_length[0] = (uint8_t)(length & 0xff);
    rc_length[1] = (uint8_t)((length >> 8) & 0xff);
    ret = dp_mst_aux_write(l->mgr, aux, SYNAPTICS_RC_LENGTH, rc_length, sizeof(rc_length));
    /* write rc cmd */
    rc_cmd = (uint8_t)(cmd | 0x80);
    ret = dp_mst_aux_write(l->mgr, aux, SYNAPTICS_RC_COMMAND, &rc_cmd, sizeof(rc_cmd));
    if (ret < 0) {
        RERR(l->d, "dp: MST: Synaptics RC command 0x%02x: write failed (%d)", cmd, ret);
        return 0;
    }
    /* poll until active is 0 */
    for (i = 0; i < 10; i++) {
        dp_mst_aux_read(l->mgr, aux, SYNAPTICS_RC_COMMAND, &rc_cmd, sizeof(rc_cmd));
        if (rc_cmd == cmd)
            break;   /* active is 0 */
        l->d->os.msleep(l->d->os.ctx, 10);
    }
    /* read rc result */
    dp_mst_aux_read(l->mgr, aux, SYNAPTICS_RC_RESULT, &rc_result, sizeof(rc_result));
    success = rc_result == 0;
    if (success && !is_write_cmd)
        dp_mst_aux_read(l->mgr, aux, SYNAPTICS_RC_DATA, data, length);   /* read rc data */
    RDBG(l->d, "dp: MST: Synaptics RC command 0x%02x: success = %d", cmd, success);
    return success;
}

/* apply_synaptics_fifo_reset_wa: the hub's SDP FIFO reset (bit 1 of three
 * registers cleared, then set), its steps as amdgpu numbers them */
static void apply_synaptics_fifo_reset_wa(rdna3_dp_link *l, int aux)
{
    uint8_t data[16];
    zero(data, sizeof(data));
    /* Step 2 */
    data[0] = 'P';
    data[1] = 'R';
    data[2] = 'I';
    data[3] = 'U';
    data[4] = 'S';
    if (!execute_synaptics_rc_command(l, aux, 1, 0x01, 5, 0, data))
        return;
    /* Step 3 and 4 */
    if (!execute_synaptics_rc_command(l, aux, 0, 0x31, 4, 0x220998, data))
        return;
    data[0] &= (uint8_t)~(1u << 1);   /* set bit 1 to 0 */
    if (!execute_synaptics_rc_command(l, aux, 1, 0x21, 4, 0x220998, data))
        return;
    if (!execute_synaptics_rc_command(l, aux, 0, 0x31, 4, 0x220D98, data))
        return;
    data[0] &= (uint8_t)~(1u << 1);
    if (!execute_synaptics_rc_command(l, aux, 1, 0x21, 4, 0x220D98, data))
        return;
    if (!execute_synaptics_rc_command(l, aux, 0, 0x31, 4, 0x221198, data))
        return;
    data[0] &= (uint8_t)~(1u << 1);
    if (!execute_synaptics_rc_command(l, aux, 1, 0x21, 4, 0x221198, data))
        return;
    /* Step 3 and 5 */
    if (!execute_synaptics_rc_command(l, aux, 0, 0x31, 4, 0x220998, data))
        return;
    data[0] |= (uint8_t)(1u << 1);    /* set bit 1 to 1 */
    if (!execute_synaptics_rc_command(l, aux, 1, 0x21, 4, 0x220998, data))
        return;
    if (!execute_synaptics_rc_command(l, aux, 0, 0x31, 4, 0x220D98, data))
        return;
    data[0] |= (uint8_t)(1u << 1);
    /* (amdgpu never writes 0x220D98 back with the bit set: kept as it is) */
    if (!execute_synaptics_rc_command(l, aux, 0, 0x31, 4, 0x221198, data))
        return;
    data[0] |= (uint8_t)(1u << 1);
    if (!execute_synaptics_rc_command(l, aux, 1, 0x21, 4, 0x221198, data))
        return;
    /* Step 6 */
    if (!execute_synaptics_rc_command(l, aux, 1, 0x02, 0, 0, 0))
        return;
    RDBG(l->d, "dp: MST: Synaptics FIFO reset done");
}

/* MST Dock */
static const uint8_t SYNAPTICS_DEVICE_ID[] = "SYNA";

/* write_dsc_enable_synaptics_non_virtual_dpcd_mst */
static int write_dsc_enable_synaptics_non_virtual_dpcd_mst(rdna3_dp_link *l, int aux, uint8_t enable)
{
    int ret = 0;
    RDBG(l->d, "dp: MST_DSC Configure DSC to non-virtual dpcd synaptics");
    if (enable) {
        /* DSC left on by the previous boot can get the hub stuck: its SDP
         * FIFO reset before the first stream */
        if (!l->link_active && bytes_eq(l->dpcd_caps.branch_dev_name, SYNAPTICS_DEVICE_ID, 4))
            apply_synaptics_fifo_reset_wa(l, aux);
        ret = dp_mst_aux_write(l->mgr, aux, RD_DPCD_DSC_ENABLE, &enable, 1);
        RLOG(l->d, "dp: MST_DSC Send DSC enable to synaptics");
    } else {
        /* no virtual DPCD on the hub: DSC off only with the whole link
         * (garbage on the monitor otherwise) */
        if (!l->link_active) {
            ret = dp_mst_aux_write(l->mgr, aux, RD_DPCD_DSC_ENABLE, &enable, 1);
            RLOG(l->d, "dp: MST_DSC Send DSC disable to synaptics");
        }
    }
    return ret > 0;
}

int dcn32_dp_mst_write_dsc_enable(rdna3_dp_link *l, int enable)
{
    const uint8_t DSC_DISABLE = 0, DSC_DECODING = 0x01, DSC_PASSTHROUGH = 0x02;
    const rdna3_mst_connector *aconnector = &l->mst_conn;
    uint8_t enable_dsc = enable ? DSC_DECODING : DSC_DISABLE;
    uint8_t enable_passthrough = enable ? DSC_PASSTHROUGH : DSC_DISABLE;
    int ret = 0;

    if (!l->mgr || aconnector->dsc_aux == RD_MST_AUX_NONE)
        return 0;
    /* apply w/a to synaptics */
    if (dcn32_dp_mst_needs_dsc_aux_workaround(l) && (aconnector->mst_downstream_port_present & 0x7) != 0x3)
        return write_dsc_enable_synaptics_non_virtual_dpcd_mst(l, aconnector->dsc_aux, enable_dsc);
    if (enable) {
        if (aconnector->passthrough_aux != RD_MST_AUX_NONE) {
            ret = dp_mst_aux_write(l->mgr, aconnector->passthrough_aux, RD_DPCD_DSC_ENABLE, &enable_passthrough, 1);
            RDBG(l->d, "dp: MST_DSC Sent DSC pass-through enable to virtual dpcd port, ret = %d", ret);
        }
        ret = dp_mst_aux_write(l->mgr, aconnector->dsc_aux, RD_DPCD_DSC_ENABLE, &enable_dsc, 1);
        RLOG(l->d, "dp: MST_DSC Sent DSC decoding enable to %s port, ret = %d",
             aconnector->passthrough_aux != RD_MST_AUX_NONE ? "remote RX" : "virtual dpcd", ret);
    } else {
        ret = dp_mst_aux_write(l->mgr, aconnector->dsc_aux, RD_DPCD_DSC_ENABLE, &enable_dsc, 1);
        RLOG(l->d, "dp: MST_DSC Sent DSC decoding disable to %s port, ret = %d",
             aconnector->passthrough_aux != RD_MST_AUX_NONE ? "remote RX" : "virtual dpcd", ret);
        if (aconnector->passthrough_aux != RD_MST_AUX_NONE) {
            ret = dp_mst_aux_write(l->mgr, aconnector->passthrough_aux, RD_DPCD_DSC_ENABLE, &enable_passthrough, 1);
            RDBG(l->d, "dp: MST_DSC Sent DSC pass-through disable to virtual dpcd port, ret = %d", ret);
        }
    }
    return ret > 0;
}

/* ---- adaptive sync through a PCON (amdgpu_dm_helpers.c) ---- */

/* dm_is_freesync_pcon_whitelist */
static int dm_is_freesync_pcon_whitelist(uint32_t branch_dev_id)
{
    switch (branch_dev_id) {
    case DP_BRANCH_DEVICE_ID_0060AD:
    case DP_BRANCH_DEVICE_ID_00E04C:
    case DP_BRANCH_DEVICE_ID_90CC24:
        return 1;
    default:
        return 0;
    }
}

int dcn32_dp_adaptive_sync_support_type(const rdna3_dp_link *l)
{
    int as_type = RD_ADAPTIVE_SYNC_TYPE_NONE;
    switch (l->dpcd_caps.dongle_type) {
    case RD_DONGLE_DP_HDMI:
        /* ADAPTIVE_SYNC_SDP_SUPPORT of DP_DPRX_FEATURE_ENUMERATION_LIST_CONT_1 */
        if ((l->dpcd_caps.adaptive_sync_caps & 0x01) && l->dpcd_caps.allow_invalid_msa_timing_param &&
            dm_is_freesync_pcon_whitelist(l->dpcd_caps.branch_dev_id))
            as_type = RD_FREESYNC_TYPE_PCON_IN_WHITELIST;
        break;
    default:
        break;
    }
    return as_type;
}

/* ---- the atomic check of an MST stream (amdgpu_dm.c) ---- */

/* dm_update_mst_vcpi_slots_for_dsc: the PBN compute_mst_dsc_configs_for_state
 * gave each stream of a DSC hub, the DSC flag of its payload */
static int dm_update_mst_vcpi_slots_for_dsc(rdna3_mst_atomic_state *state, rdna3_mst_dc_state *dc_state,
                                            const rdna3_mst_fairness_vars *vars)
{
    uint32_t i, j, pbn_div;
    int pbn = 0, slot_num, vcpi, ret;
    for (i = 0; i < dc_state->stream_count; i++) {
        rdna3_mst_stream *stream = dc_state->streams[i];
        const rdna3_mst_connector *aconnector = stream->aconnector;
        rdna3_mst_atomic *st;
        if (!aconnector || aconnector->port < 0 || !stream->link->mgr)
            continue;
        pbn_div = dcn32_dp_mst_pbn_divider(stream->link);
        /* pbn is calculated by compute_mst_dsc_configs_for_state */
        for (j = 0; j < dc_state->stream_count; j++)
            if (vars[j].aconnector == aconnector) {
                pbn = vars[j].pbn;
                break;
            }
        if (j == dc_state->stream_count || pbn_div == 0)
            continue;
        slot_num = (int)(((uint32_t)pbn + pbn_div - 1) / pbn_div);
        st = get_mst_topology_state(state, stream->link);
        if (!st)
            return RDNA3_EINVAL;
        if (!stream->timing.dsc) {
            stream->pbn = pbn;
            stream->vcpi_slots = slot_num;
            ret = dp_mst_atomic_enable_dsc(stream->link->mgr, st, aconnector->port, stream->pbn, 0);
            if (ret < 0)
                return ret;
            continue;
        }
        vcpi = dp_mst_atomic_enable_dsc(stream->link->mgr, st, aconnector->port, pbn, 1);
        if (vcpi < 0)
            return vcpi;
        stream->pbn = pbn;
        stream->vcpi_slots = vcpi;
    }
    return 0;
}

int dcn32_dp_mst_mode_fits(rdna3_dp_link *l, const rdna3_mode *m, uint32_t *pbn, uint32_t *slots, int *dsc,
                           rdna3_dsc_config *cfg, int *mode_changed)
{
    rdna3_mst_stream cur, nw;
    rdna3_mst_dc_state cur_state, new_state;
    rdna3_mst_atomic_state state;
    rdna3_mst_fairness_vars vars[RD_DCN_MAX_PIPES];
    rdna3_mst_atomic *st;
    int ret;

    *dsc = 0;
    *mode_changed = 1;
    zero(cfg, sizeof(*cfg));
    if (!l->mgr || l->mst_port < 0 || l->mst_port >= RD_MST_MAX_PORTS || !l->mgr->port[l->mst_port].used)
        return RDNA3_EINVAL;
    if (!l->mst_conn.valid || l->mst_conn.port != l->mst_port)
        dcn32_dp_mst_detect_dsc(l, &l->mst_conn, l->mst_port);
    /* create_validate_stream_for_sink: the stream, its DSC from the port
     * support check */
    zero(&nw, sizeof(nw));
    nw.link = l;
    nw.aconnector = &l->mst_conn;
    nw.is_mst = 1;
    dcn32_dp_dsc_timing(m, 8, &nw.timing);
    ret = dcn32_dp_mst_is_port_support_mode(l, &l->mst_conn, &nw.timing);
    if (ret)
        return ret;
    nw.enable = nw.active = nw.mode_changed = 1;
    /* dm_encoder_helper_atomic_check: drm_dp_calc_pbn_mode(clock, bpp << 4)
     * and the time slots, pbn_div = dm_mst_get_pbn_divider */
    nw.pbn = drm_dp_calc_pbn_mode((int)m->pixel_khz, 24 << 4);
    zero(&state, sizeof(state));
    zero(&cur_state, sizeof(cur_state));
    zero(&new_state, sizeof(new_state));
    zero(vars, sizeof(vars));
    /* dc->current_state: the stream this link runs */
    if (l->mst_cur_valid) {
        zero(&cur, sizeof(cur));
        cur.link = l;
        cur.aconnector = &l->mst_conn;
        cur.is_mst = 1;
        cur.timing = l->mst_cur_timing;
        cur_state.streams[cur_state.stream_count++] = &cur;
    }
    state.current = &cur_state;
    new_state.streams[new_state.stream_count++] = &nw;
    st = get_mst_topology_state(&state, l);
    if (!st)
        return RDNA3_EINVAL;
    nw.vcpi_slots = dp_mst_atomic_find_time_slots(l->mgr, st, l->mst_port, nw.pbn);
    if (nw.vcpi_slots < 0)
        return nw.vcpi_slots;
    /* amdgpu_dm_atomic_check: the DSC fairness when DC encodes DSC
     * (dc_resource_is_dsc_encoding_supported; here rdna3dsc=1) */
    if (!l->dsc_policy)
        goto check;
    ret = dcn32_dp_mst_pre_validate_dsc(&state, &new_state, vars);
    if (ret)
        return ret;
    if (!nw.mode_changed) {
        /* the CRTC keeps the stream it runs: its payload and DSC as they are */
        *pbn = l->mst_pbn;
        *slots = l->mst_slots;
        *dsc = l->stream_dsc;
        if (l->stream_dsc)
            *cfg = l->dsc_cfg;
        *mode_changed = 0;
        return RDNA3_OK;
    }
    zero(vars, sizeof(vars));
    ret = dcn32_dp_mst_compute_dsc_configs_for_state(&state, &new_state, vars);
    if (ret) {
        RDBG(l->d, "dp: MST_DSC compute_mst_dsc_configs_for_state() failed (%d)", ret);
        return ret;
    }
    ret = dm_update_mst_vcpi_slots_for_dsc(&state, &new_state, vars);
    if (ret)
        return ret;
check:
    ret = dcn32_dp_mst_atomic_check(&state);
    if (ret)
        return ret;
    *pbn = (uint32_t)nw.pbn;
    *slots = (uint32_t)nw.vcpi_slots;
    *dsc = nw.timing.dsc;
    if (nw.timing.dsc)
        *cfg = nw.timing.dsc_cfg;
    *mode_changed = nw.mode_changed;
    return RDNA3_OK;
}

/* ---- a running MST stream's payload resized (link_dpms.c) ---- */

/* dm_helpers_dp_mst_write_payload_allocation_table for a stream that keeps
 * its VCPI: the VC payload ID table entry resized (the DRM time slots the
 * commit gave it); DC's table with the VCP's new slot count (amdgpu's
 * fill_dc_mst_payload_table_from_drm would list the VCP twice) */
static int write_payload_allocation_table_resize(rdna3_dp_link *l, rdna3_mst_alloc_table *proposed)
{
    rdna3_mst_mgr *mgr = l->mgr;
    uint32_t i, vcpi = (uint32_t)mgr->port[l->mst_port].vcpi;
    if (dp_mst_update_payload_part1(mgr, l->mst_port, dm_helpers_construct_old_payload(mgr, l->mst_port)))
        return 0;
    zero(proposed, sizeof(*proposed));
    for (i = 0; i < l->mst_alloc.stream_count && i < RD_MST_MAX_STREAMS; i++)
        if (l->mst_alloc.a[i].vcp_id > 0 && l->mst_alloc.a[i].slot_count > 0) {
            proposed->a[proposed->stream_count].vcp_id = l->mst_alloc.a[i].vcp_id;
            proposed->a[proposed->stream_count].slot_count =
                l->mst_alloc.a[i].vcp_id == vcpi ? (uint32_t)mgr->port[l->mst_port].time_slots : l->mst_alloc.a[i].slot_count;
            proposed->a[proposed->stream_count].fe = -1;
            proposed->stream_count++;
        }
    return 1;
}

/* the commit's PBN for the stream into the manager (drm_dp_atomic_find_time_slots) */
static int set_stream_time_slots(rdna3_dp_link *l)
{
    int slots;
    l->mgr->pbn_div = dcn32_dp_mst_pbn_divider(l);
    slots = dp_mst_find_time_slots(l->mgr, l->mst_port, (int)l->mst_req_pbn);
    if (slots < 0) {
        RERR(l->d, "dp: MST: no time slots for PBN %u", l->mst_req_pbn);
        return RDNA3_EINVAL;
    }
    l->mst_pbn = l->mst_req_pbn;
    l->mst_slots = (uint32_t)slots;
    return RDNA3_OK;
}

int dcn32_dp_mst_reduce_payload(rdna3_dp_link *l, uint32_t bw_in_kbps)
{
    rdna3_mst_alloc_table proposed;
    rd_fixpt avg_time_slots_per_mtp, pbn, pbn_per_slot;

    if (!l->mst || !l->mgr || l->mst_port < 0 || !l->link_active)
        return RDNA3_EINVAL;
    zero(&proposed, sizeof(proposed));
    /* decrease throttled vcp size */
    pbn_per_slot = get_pbn_per_slot(l);
    if (pbn_per_slot.value == 0)
        return RDNA3_EINVAL;
    pbn = get_pbn_from_bw_in_kbps(bw_in_kbps);
    avg_time_slots_per_mtp = dc_fixpt_div(pbn, pbn_per_slot);
    log_vcp_x_y(l, avg_time_slots_per_mtp);
    enc1_stream_encoder_set_throttled_vcp_size(l, avg_time_slots_per_mtp);
    if (set_stream_time_slots(l))
        return RDNA3_EINVAL;
    /* send ALLOCATE_PAYLOAD sideband message with updated pbn */
    if (dp_mst_update_payload_part2(l->mgr, l->mst_port))
        RERR(l->d, "dp: MST: ALLOCATE_PAYLOAD for port %u failed", l->mgr->port[l->mst_port].port_num);
    /* notify immediate branch device table update */
    if (write_payload_allocation_table_resize(l, &proposed))
        update_mst_stream_alloc_table(l, (int)l->fe, &proposed);   /* software state */
    else
        RERR(l->d, "dp: MST: failed to update the MST allocation table for DIG %u", l->fe);
    RLOG(l->d, "dp: MST reduce payload: %u stream(s): VCP %u, %u slots (PBN %u)", l->mst_alloc.stream_count,
         l->mst_alloc.a[0].vcp_id, l->mst_alloc.a[0].slot_count, l->mst_pbn);
    if (link_is_unknown_encoding(&l->cur_link_settings)) {
        RERR(l->d, "dp: MST: unknown encoding format");
        return RDNA3_EIO;
    }
    /* update mst stream allocation table hardware state */
    update_dio_stream_allocation_table(l);
    /* poll for immediate branch device ACT handled */
    l->mst_act = dm_helpers_dp_mst_poll_for_allocation_change_trigger(l) == ACT_SUCCESS;
    return RDNA3_OK;
}

int dcn32_dp_mst_increase_payload(rdna3_dp_link *l, uint32_t bw_in_kbps)
{
    rdna3_mst_alloc_table proposed;
    rd_fixpt avg_time_slots_per_mtp, pbn, pbn_per_slot;
    int ret;

    if (!l->mst || !l->mgr || l->mst_port < 0 || !l->link_active)
        return RDNA3_EINVAL;
    zero(&proposed, sizeof(proposed));
    if (set_stream_time_slots(l))
        return RDNA3_EINVAL;
    /* notify immediate branch device table update */
    if (write_payload_allocation_table_resize(l, &proposed))
        update_mst_stream_alloc_table(l, (int)l->fe, &proposed);   /* software state */
    RLOG(l->d, "dp: MST increase payload: %u stream(s): VCP %u, %u slots (PBN %u)", l->mst_alloc.stream_count,
         l->mst_alloc.a[0].vcp_id, l->mst_alloc.a[0].slot_count, l->mst_pbn);
    if (link_is_unknown_encoding(&l->cur_link_settings)) {
        RERR(l->d, "dp: MST: unknown encoding format");
        return RDNA3_EIO;
    }
    /* update mst stream allocation table hardware state */
    update_dio_stream_allocation_table(l);
    /* poll for immediate branch device ACT handled */
    ret = dm_helpers_dp_mst_poll_for_allocation_change_trigger(l);
    l->mst_act = ret == ACT_SUCCESS;
    /* send ALLOCATE_PAYLOAD sideband message with updated pbn */
    if (ret != ACT_LINK_LOST && dp_mst_update_payload_part2(l->mgr, l->mst_port))
        RERR(l->d, "dp: MST: ALLOCATE_PAYLOAD for port %u failed", l->mgr->port[l->mst_port].port_num);
    /* increase throttled vcp size */
    pbn = get_pbn_from_bw_in_kbps(bw_in_kbps);
    pbn_per_slot = get_pbn_per_slot(l);
    if (pbn_per_slot.value == 0)
        return RDNA3_EINVAL;
    avg_time_slots_per_mtp = dc_fixpt_div(pbn, pbn_per_slot);
    log_vcp_x_y(l, avg_time_slots_per_mtp);
    enc1_stream_encoder_set_throttled_vcp_size(l, avg_time_slots_per_mtp);
    return RDNA3_OK;
}

int dcn32_dp_mst_update_stream_dsc(rdna3_dp_link *l, const rdna3_mode *m, const rdna3_dsc_config *cfg, uint32_t pbn)
{
    rdna3_dsc_config old_dsc_cfg = l->dsc_cfg;
    uint32_t old_pbn = l->mst_req_pbn, old_kbps, new_kbps;
    int enable_dsc = cfg->num_slices_h != 0 && cfg->num_slices_v != 0, ret;

    if (!l->mst || !l->mgr || !l->link_active || !l->mst_cur_valid || !l->stream_dsc)
        return RDNA3_EINVAL;
    /* copy_stream_update_to_stream: a DSC config DC validates (the stream
     * keeps DSC: enabling or disabling it is a mode set here) */
    if (!enable_dsc || cfg->num_slices_h % (l->odm_opp_cnt > 1 ? l->odm_opp_cnt : 1))
        return RDNA3_EINVAL;
    old_kbps = dcn32_dp_mode_kbps(m, 8, &old_dsc_cfg);
    l->dsc_cfg = *cfg;
    new_kbps = dcn32_dp_mode_kbps(m, 8, &l->dsc_cfg);
    /* commit_planes_do_stream_update: dsc_config, then mst_bw_update */
    if (!dcn32_dp_link_update_dsc_config(l, m)) {
        l->dsc_cfg = old_dsc_cfg;
        return RDNA3_EINVAL;
    }
    l->mst_req_pbn = pbn;
    ret = new_kbps > old_kbps ? dcn32_dp_mst_increase_payload(l, new_kbps) : dcn32_dp_mst_reduce_payload(l, new_kbps);
    if (ret) {
        RERR(l->d, "dp: MST: payload update for %u kbps failed (%d)", new_kbps, ret);
        l->mst_req_pbn = old_pbn;
        return ret;
    }
    l->mst_cur_timing.dsc_cfg = l->dsc_cfg;
    RLOG(l->d, "dp: MST stream update: DSC %u.%04u bpp, %u kbps, PBN %u, %u slots", l->dsc_cfg.bits_per_pixel / 16,
         (l->dsc_cfg.bits_per_pixel % 16) * 10000 / 16, new_kbps, l->mst_pbn, l->mst_slots);
    return RDNA3_OK;
}
