/*
 * dp_mst.c - see dp_mst.h. A port of the DRM DP MST topology helper
 * (drm_dp_mst_topology.c, drm_dp_helper.c; Linux v6.12, MIT) and the polled
 * sideband reply handling of amdgpu_dm_mst_types.c; every function names
 * the one it ports. One down request is in flight at a time, as in DRM;
 * the reply is polled through the ESI bytes (amdgpu's poll_hpd_irq) instead
 * of waiting for the HPD short pulse.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dp_mst.h"
#include "dcaux.h"

static void zero(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--)
        *b++ = 0;
}

static void copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *a = (uint8_t *)dst;
    const uint8_t *b = (const uint8_t *)src;
    while (n--)
        *a++ = *b++;
}

/* drm_dp_dpcd_read / drm_dp_dpcd_write on the manager's AUX channel:
 * bytes transferred or a negative error */
static int dpcd_read(rdna3_mst_mgr *mgr, uint32_t a, uint8_t *b, uint32_t n)
{
    return dcaux_dpcd_read(mgr->d, mgr->aux, a, b, n);
}

static int dpcd_write(rdna3_mst_mgr *mgr, uint32_t a, const uint8_t *b, uint32_t n)
{
    return dcaux_dpcd_write(mgr->d, mgr->aux, a, b, n);
}

static int dpcd_writeb(rdna3_mst_mgr *mgr, uint32_t a, uint8_t v)
{
    return dpcd_write(mgr, a, &v, 1);
}

/* ---- sideband messages ---- */

/* drm_dp_msg_header_crc4 */
static uint8_t drm_dp_msg_header_crc4(const uint8_t *data, uint32_t num_nibbles)
{
    uint8_t bitmask = 0x80, bitshift = 7, array_index = 0, remainder = 0;
    int number_of_bits = (int)num_nibbles * 4;
    while (number_of_bits != 0) {
        number_of_bits--;
        remainder = (uint8_t)(remainder << 1);
        remainder = (uint8_t)(remainder | ((data[array_index] & bitmask) >> bitshift));
        bitmask >>= 1;
        bitshift--;
        if (bitmask == 0) {
            bitmask = 0x80;
            bitshift = 7;
            array_index++;
        }
        if ((remainder & 0x10) == 0x10)
            remainder ^= 0x13;
    }
    number_of_bits = 4;
    while (number_of_bits != 0) {
        number_of_bits--;
        remainder = (uint8_t)(remainder << 1);
        if ((remainder & 0x10) != 0)
            remainder ^= 0x13;
    }
    return remainder;
}

/* drm_dp_msg_data_crc4 */
static uint8_t drm_dp_msg_data_crc4(const uint8_t *data, uint8_t number_of_bytes)
{
    uint8_t bitmask = 0x80, bitshift = 7, array_index = 0;
    int number_of_bits = number_of_bytes * 8;
    uint16_t remainder = 0;
    while (number_of_bits != 0) {
        number_of_bits--;
        remainder = (uint16_t)(remainder << 1);
        remainder = (uint16_t)(remainder | ((data[array_index] & bitmask) >> bitshift));
        bitmask >>= 1;
        bitshift--;
        if (bitmask == 0) {
            bitmask = 0x80;
            bitshift = 7;
            array_index++;
        }
        if ((remainder & 0x100) == 0x100)
            remainder ^= 0xd5;
    }
    number_of_bits = 8;
    while (number_of_bits != 0) {
        number_of_bits--;
        remainder = (uint16_t)(remainder << 1);
        if ((remainder & 0x100) != 0)
            remainder ^= 0xd5;
    }
    return (uint8_t)(remainder & 0xff);
}

/* drm_dp_calc_sb_hdr_size */
static uint8_t drm_dp_calc_sb_hdr_size(const rdna3_mst_sb_hdr *hdr)
{
    return (uint8_t)(3 + hdr->lct / 2);
}

/* drm_dp_encode_sideband_msg_hdr */
static void drm_dp_encode_sideband_msg_hdr(const rdna3_mst_sb_hdr *hdr, uint8_t *buf, int *len)
{
    int idx = 0, i;
    uint8_t crc4;
    buf[idx++] = (uint8_t)(((hdr->lct & 0xf) << 4) | (hdr->lcr & 0xf));
    for (i = 0; i < hdr->lct / 2; i++)
        buf[idx++] = hdr->rad[i];
    buf[idx++] = (uint8_t)((hdr->broadcast ? 0x80 : 0) | (hdr->path_msg ? 0x40 : 0) | (hdr->msg_len & 0x3f));
    buf[idx++] = (uint8_t)((hdr->somt ? 0x80 : 0) | (hdr->eomt ? 0x40 : 0) | (hdr->seqno << 4));
    crc4 = drm_dp_msg_header_crc4(buf, (uint32_t)(idx * 2) - 1);
    buf[idx - 1] = (uint8_t)(buf[idx - 1] | (crc4 & 0xf));
    *len = idx;
}

/* drm_dp_decode_sideband_msg_hdr */
static int drm_dp_decode_sideband_msg_hdr(rdna3_mst_mgr *mgr, rdna3_mst_sb_hdr *hdr, const uint8_t *buf, int buflen,
                                          uint8_t *hdrlen)
{
    uint8_t crc4, len, idx;
    int i;
    if (buf[0] == 0)
        return 0;
    len = (uint8_t)(3 + ((buf[0] & 0xf0) >> 4) / 2);
    if (len > buflen)
        return 0;
    crc4 = drm_dp_msg_header_crc4(buf, (uint32_t)(len * 2) - 1);
    if ((crc4 & 0xf) != (buf[len - 1] & 0xf)) {
        RDBG(mgr->d, "mst: crc4 mismatch 0x%x 0x%x", crc4, buf[len - 1]);
        return 0;
    }
    hdr->lct = (uint8_t)((buf[0] & 0xf0) >> 4);
    hdr->lcr = (uint8_t)(buf[0] & 0xf);
    idx = 1;
    for (i = 0; i < hdr->lct / 2; i++)
        hdr->rad[i] = buf[idx++];
    hdr->broadcast = (buf[idx] >> 7) & 1;
    hdr->path_msg = (buf[idx] >> 6) & 1;
    hdr->msg_len = (uint8_t)(buf[idx] & 0x3f);
    idx++;
    hdr->somt = (buf[idx] >> 7) & 1;
    hdr->eomt = (buf[idx] >> 6) & 1;
    hdr->seqno = (uint8_t)((buf[idx] >> 4) & 1);
    idx++;
    *hdrlen = idx;
    return 1;
}

/* struct drm_dp_sideband_msg_req_body (the requests sent here) */
typedef struct sb_req {
    uint8_t req_type;
    uint8_t port_number;
    struct { uint8_t vcpi; uint16_t pbn; uint8_t number_sdp_streams; uint8_t sdp_stream_sink[16]; } allocate_payload;
    struct { uint32_t dpcd_address; uint8_t num_bytes; } dpcd_read;
    struct {
        uint8_t num_transactions;
        struct { uint8_t i2c_dev_id, num_bytes; const uint8_t *bytes; uint8_t no_stop_bit, i2c_transaction_delay; } t[4];
        uint8_t read_i2c_device_id, num_bytes_read;
    } i2c_read;
} sb_req;

/* drm_dp_encode_sideband_req */
static void drm_dp_encode_sideband_req(const sb_req *req, rdna3_mst_sb_tx *raw)
{
    uint32_t idx = 0, i, k;
    uint8_t *buf = raw->msg;
    buf[idx++] = (uint8_t)(req->req_type & 0x7f);
    switch (req->req_type) {
    case RD_DP_ENUM_PATH_RESOURCES:
        buf[idx++] = (uint8_t)((req->port_number & 0xf) << 4);
        break;
    case RD_DP_ALLOCATE_PAYLOAD:
        buf[idx++] = (uint8_t)((req->port_number & 0xf) << 4 | (req->allocate_payload.number_sdp_streams & 0xf));
        buf[idx++] = (uint8_t)(req->allocate_payload.vcpi & 0x7f);
        buf[idx++] = (uint8_t)(req->allocate_payload.pbn >> 8);
        buf[idx++] = (uint8_t)(req->allocate_payload.pbn & 0xff);
        for (i = 0; i < req->allocate_payload.number_sdp_streams / 2u; i++)
            buf[idx++] = (uint8_t)(((req->allocate_payload.sdp_stream_sink[i * 2] & 0xf) << 4) |
                                   (req->allocate_payload.sdp_stream_sink[i * 2 + 1] & 0xf));
        if (req->allocate_payload.number_sdp_streams & 1) {
            i = req->allocate_payload.number_sdp_streams - 1u;
            buf[idx++] = (uint8_t)((req->allocate_payload.sdp_stream_sink[i] & 0xf) << 4);
        }
        break;
    case RD_DP_REMOTE_DPCD_READ:
        buf[idx] = (uint8_t)((req->port_number & 0xf) << 4);
        buf[idx] = (uint8_t)(buf[idx] | (((req->dpcd_read.dpcd_address & 0xf0000) >> 16) & 0xf));
        idx++;
        buf[idx++] = (uint8_t)((req->dpcd_read.dpcd_address & 0xff00) >> 8);
        buf[idx++] = (uint8_t)(req->dpcd_read.dpcd_address & 0xff);
        buf[idx++] = req->dpcd_read.num_bytes;
        break;
    case RD_DP_REMOTE_I2C_READ:
        buf[idx++] = (uint8_t)(((req->port_number & 0xf) << 4) | (req->i2c_read.num_transactions & 0x3));
        for (i = 0; i < (req->i2c_read.num_transactions & 0x3u); i++) {
            buf[idx++] = (uint8_t)(req->i2c_read.t[i].i2c_dev_id & 0x7f);
            buf[idx++] = req->i2c_read.t[i].num_bytes;
            for (k = 0; k < req->i2c_read.t[i].num_bytes; k++)
                buf[idx++] = req->i2c_read.t[i].bytes[k];
            buf[idx++] = (uint8_t)(((req->i2c_read.t[i].no_stop_bit & 0x1) << 4) |
                                   (req->i2c_read.t[i].i2c_transaction_delay & 0xf));
        }
        buf[idx++] = (uint8_t)(req->i2c_read.read_i2c_device_id & 0x7f);
        buf[idx++] = req->i2c_read.num_bytes_read;
        break;
    default:   /* LINK_ADDRESS, CLEAR_PAYLOAD_ID_TABLE: the type alone */
        break;
    }
    raw->cur_len = idx;
}

/* drm_dp_crc_sideband_chunk_req */
static void drm_dp_crc_sideband_chunk_req(uint8_t *msg, uint8_t len)
{
    msg[len] = drm_dp_msg_data_crc4(msg, len);
}

/* drm_dp_sideband_msg_set_header */
static int drm_dp_sideband_msg_set_header(rdna3_mst_sb_rx *msg, const rdna3_mst_sb_hdr *hdr, uint8_t hdrlen)
{
    /* out-of-order messages or parts of a failed transaction */
    if (!hdr->somt && !msg->have_somt)
        return 0;
    msg->curchunk_idx = 0;
    msg->curchunk_len = hdr->msg_len;
    msg->curchunk_hdrlen = hdrlen;
    /* an SOMT already: do not parse */
    if (hdr->somt && msg->have_somt)
        return 0;
    if (hdr->somt) {
        msg->initial_hdr = *hdr;
        msg->have_somt = 1;
    }
    if (hdr->eomt)
        msg->have_eomt = 1;
    return 1;
}

/* drm_dp_sideband_append_payload */
static int drm_dp_sideband_append_payload(rdna3_mst_mgr *mgr, rdna3_mst_sb_rx *msg, const uint8_t *replybuf,
                                          uint8_t replybuflen)
{
    uint8_t crc4;
    if (msg->curchunk_idx + replybuflen > sizeof(msg->chunk))
        return 0;
    copy(&msg->chunk[msg->curchunk_idx], replybuf, replybuflen);
    msg->curchunk_idx = (uint8_t)(msg->curchunk_idx + replybuflen);
    if (msg->curchunk_idx >= msg->curchunk_len) {
        crc4 = drm_dp_msg_data_crc4(msg->chunk, (uint8_t)(msg->curchunk_len - 1));
        if (crc4 != msg->chunk[msg->curchunk_len - 1]) {
            RDBG(mgr->d, "mst: wrong body crc %02x != %02x", crc4, msg->chunk[msg->curchunk_len - 1]);
            mgr->bad_replies++;
        }
        if (msg->curlen + msg->curchunk_len - 1 > sizeof(msg->msg))
            return 0;
        copy(&msg->msg[msg->curlen], msg->chunk, msg->curchunk_len - 1u);
        msg->curlen += msg->curchunk_len - 1u;
    }
    return 1;
}

/* drm_dp_sideband_parse_link_address */
static int drm_dp_sideband_parse_link_address(const rdna3_mst_sb_rx *raw, rdna3_mst_reply *rep)
{
    uint32_t idx = 1, i;
    copy(rep->link_addr.guid, &raw->msg[idx], 16);
    idx += 16;
    rep->link_addr.nports = (uint8_t)(raw->msg[idx] & 0xf);
    idx++;
    if (idx > raw->curlen)
        return 0;
    for (i = 0; i < rep->link_addr.nports; i++) {
        rdna3_mst_port_reply *p = &rep->link_addr.ports[i];
        if (raw->msg[idx] & 0x80)
            p->input_port = 1;
        p->peer_device_type = (uint8_t)((raw->msg[idx] >> 4) & 0x7);
        p->port_number = (uint8_t)(raw->msg[idx] & 0xf);
        idx++;
        if (idx > raw->curlen)
            return 0;
        p->mcs = (raw->msg[idx] >> 7) & 1;
        p->ddps = (raw->msg[idx] >> 6) & 1;
        if (!p->input_port)
            p->legacy_device_plug_status = (raw->msg[idx] >> 5) & 1;
        idx++;
        if (idx > raw->curlen)
            return 0;
        if (!p->input_port) {
            p->dpcd_revision = raw->msg[idx];
            idx++;
            if (idx > raw->curlen)
                return 0;
            copy(p->peer_guid, &raw->msg[idx], 16);
            idx += 16;
            if (idx > raw->curlen)
                return 0;
            p->num_sdp_streams = (uint8_t)((raw->msg[idx] >> 4) & 0xf);
            p->num_sdp_stream_sinks = (uint8_t)(raw->msg[idx] & 0xf);
            idx++;
        }
        if (idx > raw->curlen)
            return 0;
    }
    return 1;
}

/* drm_dp_sideband_parse_remote_dpcd_read / drm_dp_sideband_parse_remote_i2c_read_ack */
static int drm_dp_sideband_parse_remote_read_ack(const rdna3_mst_sb_rx *raw, rdna3_mst_reply *rep)
{
    uint32_t idx = 1;
    rep->remote_read_ack.port_number = (uint8_t)(raw->msg[idx] & 0xf);
    idx++;
    if (idx > raw->curlen)
        return 0;
    rep->remote_read_ack.num_bytes = raw->msg[idx];
    idx++;
    if (idx > raw->curlen || idx + rep->remote_read_ack.num_bytes > sizeof(raw->msg))
        return 0;
    copy(rep->remote_read_ack.bytes, &raw->msg[idx], rep->remote_read_ack.num_bytes);
    return 1;
}

/* drm_dp_sideband_parse_enum_path_resources_ack */
static int drm_dp_sideband_parse_enum_path_resources_ack(const rdna3_mst_sb_rx *raw, rdna3_mst_reply *rep)
{
    uint32_t idx = 1;
    rep->path_resources.port_number = (uint8_t)((raw->msg[idx] >> 4) & 0xf);
    rep->path_resources.fec_capable = raw->msg[idx] & 0x1;
    idx++;
    if (idx > raw->curlen)
        return 0;
    rep->path_resources.full_payload_bw_number = (uint16_t)((raw->msg[idx] << 8) | raw->msg[idx + 1]);
    idx += 2;
    if (idx > raw->curlen)
        return 0;
    rep->path_resources.avail_payload_bw_number = (uint16_t)((raw->msg[idx] << 8) | raw->msg[idx + 1]);
    idx += 2;
    return idx <= raw->curlen;
}

/* drm_dp_sideband_parse_allocate_payload_ack */
static int drm_dp_sideband_parse_allocate_payload_ack(const rdna3_mst_sb_rx *raw, rdna3_mst_reply *rep)
{
    uint32_t idx = 1;
    rep->allocate_payload.port_number = (uint8_t)((raw->msg[idx] >> 4) & 0xf);
    idx++;
    if (idx > raw->curlen)
        return 0;
    rep->allocate_payload.vcpi = raw->msg[idx];
    idx++;
    if (idx > raw->curlen)
        return 0;
    rep->allocate_payload.allocated_pbn = (uint16_t)((raw->msg[idx] << 8) | raw->msg[idx + 1]);
    idx += 2;
    return idx <= raw->curlen;
}

/* drm_dp_sideband_parse_reply */
static int drm_dp_sideband_parse_reply(rdna3_mst_mgr *mgr, const rdna3_mst_sb_rx *raw, rdna3_mst_reply *msg)
{
    zero(msg, sizeof(*msg));
    msg->reply_type = (uint8_t)((raw->msg[0] & 0x80) >> 7);
    msg->req_type = (uint8_t)(raw->msg[0] & 0x7f);
    if (msg->reply_type == RD_DP_SIDEBAND_REPLY_NAK) {
        copy(msg->nak.guid, &raw->msg[1], 16);
        msg->nak.reason = raw->msg[17];
        msg->nak.nak_data = raw->msg[18];
        return 0;
    }
    switch (msg->req_type) {
    case RD_DP_LINK_ADDRESS: return drm_dp_sideband_parse_link_address(raw, msg);
    case RD_DP_REMOTE_DPCD_READ:
    case RD_DP_REMOTE_I2C_READ: return drm_dp_sideband_parse_remote_read_ack(raw, msg);
    case RD_DP_ENUM_PATH_RESOURCES: return drm_dp_sideband_parse_enum_path_resources_ack(raw, msg);
    case RD_DP_ALLOCATE_PAYLOAD: return drm_dp_sideband_parse_allocate_payload_ack(raw, msg);
    case RD_DP_CLEAR_PAYLOAD_ID_TABLE: return 1;   /* nothing to parse */
    default:
        RERR(mgr->d, "mst: unknown reply 0x%02x", msg->req_type);
        return 0;
    }
}

/* build_link_address */
static void build_link_address(rdna3_mst_sb_tx *msg)
{
    sb_req req;
    zero(&req, sizeof(req));
    req.req_type = RD_DP_LINK_ADDRESS;
    drm_dp_encode_sideband_req(&req, msg);
}

/* build_clear_payload_id_table */
static void build_clear_payload_id_table(rdna3_mst_sb_tx *msg)
{
    sb_req req;
    zero(&req, sizeof(req));
    req.req_type = RD_DP_CLEAR_PAYLOAD_ID_TABLE;
    drm_dp_encode_sideband_req(&req, msg);
    msg->path_msg = 1;
}

/* build_enum_path_resources */
static void build_enum_path_resources(rdna3_mst_sb_tx *msg, uint8_t port_num)
{
    sb_req req;
    zero(&req, sizeof(req));
    req.req_type = RD_DP_ENUM_PATH_RESOURCES;
    req.port_number = port_num;
    drm_dp_encode_sideband_req(&req, msg);
    msg->path_msg = 1;
}

/* build_allocate_payload */
static void build_allocate_payload(rdna3_mst_sb_tx *msg, uint8_t port_num, uint8_t vcpi, uint16_t pbn,
                                   uint8_t number_sdp_streams, const uint8_t *sdp_stream_sink)
{
    sb_req req;
    uint32_t i;
    zero(&req, sizeof(req));
    req.req_type = RD_DP_ALLOCATE_PAYLOAD;
    req.port_number = port_num;
    req.allocate_payload.vcpi = vcpi;
    req.allocate_payload.pbn = pbn;
    req.allocate_payload.number_sdp_streams = number_sdp_streams;
    for (i = 0; i < number_sdp_streams && i < 16; i++)
        req.allocate_payload.sdp_stream_sink[i] = sdp_stream_sink[i];
    drm_dp_encode_sideband_req(&req, msg);
    msg->path_msg = 1;
}

/* build_dpcd_read */
static void build_dpcd_read(rdna3_mst_sb_tx *msg, uint8_t port_num, uint32_t offset, uint8_t num_bytes)
{
    sb_req req;
    zero(&req, sizeof(req));
    req.req_type = RD_DP_REMOTE_DPCD_READ;
    req.port_number = port_num;
    req.dpcd_read.dpcd_address = offset;
    req.dpcd_read.num_bytes = num_bytes;
    drm_dp_encode_sideband_req(&req, msg);
}

/* drm_dp_send_sideband_msg: the chunk into DOWN_REQ in AUX-sized pieces */
static int drm_dp_send_sideband_msg(rdna3_mst_mgr *mgr, int up, const uint8_t *msg, int len)
{
    uint32_t regbase = up ? RD_DPCD_SIDEBAND_MSG_UP_REP_BASE : RD_DPCD_SIDEBAND_MSG_DOWN_REQ_BASE;
    int tosend, total, offset, ret, retries = 0;
retry:
    total = len;
    offset = 0;
    do {
        tosend = total < (int)mgr->max_dpcd_transaction_bytes ? total : (int)mgr->max_dpcd_transaction_bytes;
        if (tosend > 16)
            tosend = 16;
        ret = dpcd_write(mgr, regbase + (uint32_t)offset, &msg[offset], (uint32_t)tosend);
        if (ret != tosend) {
            if (retries < 5) {
                retries++;
                goto retry;
            }
            RDBG(mgr->d, "mst: failed to dpcd write %d %d", tosend, ret);
            return RDNA3_EIO;
        }
        offset += tosend;
        total -= tosend;
    } while (total > 0);
    return RDNA3_OK;
}

/* set_hdr_from_dst_qlock */
static void set_hdr_from_dst_qlock(rdna3_mst_mgr *mgr, rdna3_mst_sb_hdr *hdr, const rdna3_mst_sb_tx *txmsg)
{
    const rdna3_mst_branch *mstb = &mgr->mstb[txmsg->dst];
    uint8_t req_type = (uint8_t)(txmsg->msg[0] & 0x7f);
    int i;
    hdr->broadcast = req_type == 0x02 || req_type == 0x13 || req_type == RD_DP_CLEAR_PAYLOAD_ID_TABLE;
    hdr->path_msg = txmsg->path_msg;
    if (hdr->broadcast) {
        hdr->lct = 1;
        hdr->lcr = 6;
    } else {
        hdr->lct = mstb->lct;
        hdr->lcr = (uint8_t)(mstb->lct - 1);
    }
    for (i = 0; i < hdr->lct / 2; i++)
        hdr->rad[i] = mstb->rad[i];
}

/* process_single_tx_qlock: one chunk (48 bytes with header and CRC) */
static int process_single_tx_qlock(rdna3_mst_mgr *mgr, rdna3_mst_sb_tx *txmsg, int up)
{
    uint8_t chunk[48];
    rdna3_mst_sb_hdr hdr;
    int len, space, idx, tosend, ret;
    if (txmsg->state == RD_SB_TX_SENT)
        return 0;
    zero(&hdr, sizeof(hdr));
    if (txmsg->state == RD_SB_TX_QUEUED)
        txmsg->state = RD_SB_TX_START_SEND;
    set_hdr_from_dst_qlock(mgr, &hdr, txmsg);
    len = (int)(txmsg->cur_len - txmsg->cur_offset);
    /* 48 - 1 byte data CRC - header bytes */
    space = 48 - 1 - drm_dp_calc_sb_hdr_size(&hdr);
    tosend = len < space ? len : space;
    if (len == (int)txmsg->cur_len)
        hdr.somt = 1;
    if (space >= len)
        hdr.eomt = 1;
    hdr.msg_len = (uint8_t)(tosend + 1);
    drm_dp_encode_sideband_msg_hdr(&hdr, chunk, &idx);
    copy(&chunk[idx], &txmsg->msg[txmsg->cur_offset], (uint32_t)tosend);
    drm_dp_crc_sideband_chunk_req(&chunk[idx], (uint8_t)tosend);
    idx += tosend + 1;
    ret = drm_dp_send_sideband_msg(mgr, up, chunk, idx);
    if (ret) {
        RDBG(mgr->d, "mst: sideband msg failed to send");
        return ret;
    }
    txmsg->cur_offset += (uint32_t)tosend;
    if (txmsg->cur_offset == txmsg->cur_len) {
        txmsg->state = RD_SB_TX_SENT;
        return 1;
    }
    return 0;
}

/* drm_dp_get_mst_branch_device: the branch a reply's lct / rad names */
static int drm_dp_get_mst_branch_device(const rdna3_mst_mgr *mgr, uint8_t lct, const uint8_t *rad)
{
    int mstb = mgr->mst_primary, i, k;
    if (mstb < 0)
        return -1;
    for (i = 0; i < lct - 1; i++) {
        int shift = (i % 2) ? 0 : 4, port_num = (rad[i / 2] >> shift) & 0xf, next = -1;
        for (k = 0; k < RD_MST_MAX_PORTS; k++)
            if (mgr->port[k].used && mgr->port[k].parent == mstb && mgr->port[k].port_num == port_num) {
                next = mgr->port[k].mstb;
                break;
            }
        if (next < 0) {
            RERR(mgr->d, "mst: failed to look up the branch with lct %u, rad %02x", lct, rad[0]);
            return -1;
        }
        mstb = next;
    }
    return mstb;
}

/* drm_dp_get_one_sb_msg (down replies): 16 bytes, the header, then the rest
 * of the chunk */
static int drm_dp_get_one_sb_msg(rdna3_mst_mgr *mgr, int *mstb)
{
    uint8_t replyblock[32], hdrlen;
    rdna3_mst_sb_hdr hdr;
    rdna3_mst_sb_rx *msg = &mgr->down_rep_recv;
    uint32_t basereg = RD_DPCD_SIDEBAND_MSG_DOWN_REP_BASE;
    int len, ret, replylen, curreply;

    *mstb = -1;
    len = mgr->max_dpcd_transaction_bytes < 16 ? (int)mgr->max_dpcd_transaction_bytes : 16;
    ret = dpcd_read(mgr, basereg, replyblock, (uint32_t)len);
    if (ret != len) {
        RDBG(mgr->d, "mst: failed to read DPCD down rep %d %d", len, ret);
        return 0;
    }
    zero(&hdr, sizeof(hdr));
    if (!drm_dp_decode_sideband_msg_hdr(mgr, &hdr, replyblock, len, &hdrlen)) {
        RDBG(mgr->d, "mst: failed header %02x %02x %02x", replyblock[0], replyblock[1], replyblock[2]);
        return 0;
    }
    *mstb = drm_dp_get_mst_branch_device(mgr, hdr.lct, hdr.rad);
    if (*mstb < 0) {
        RDBG(mgr->d, "mst: reply from unknown device %u", hdr.lct);
        return 0;
    }
    if (!drm_dp_sideband_msg_set_header(msg, &hdr, hdrlen)) {
        RDBG(mgr->d, "mst: sideband msg set header failed %02x", replyblock[0]);
        return 0;
    }
    replylen = msg->curchunk_len < len - hdrlen ? msg->curchunk_len : len - hdrlen;
    if (!drm_dp_sideband_append_payload(mgr, msg, replyblock + hdrlen, (uint8_t)replylen))
        return 0;
    replylen = msg->curchunk_len + msg->curchunk_hdrlen - len;
    curreply = len;
    while (replylen > 0) {
        len = replylen < (int)mgr->max_dpcd_transaction_bytes ? replylen : (int)mgr->max_dpcd_transaction_bytes;
        if (len > 16)
            len = 16;
        ret = dpcd_read(mgr, basereg + (uint32_t)curreply, replyblock, (uint32_t)len);
        if (ret != len) {
            RDBG(mgr->d, "mst: failed to read a chunk (len %d, ret %d)", len, ret);
            return 0;
        }
        if (!drm_dp_sideband_append_payload(mgr, msg, replyblock, (uint8_t)len))
            return 0;
        curreply += len;
        replylen -= len;
    }
    return 1;
}

/* drm_dp_mst_handle_down_rep */
static void drm_dp_mst_handle_down_rep(rdna3_mst_mgr *mgr)
{
    rdna3_mst_sb_rx *msg = &mgr->down_rep_recv;
    rdna3_mst_sb_tx *txmsg = &mgr->tx;
    int mstb = -1;
    if (!drm_dp_get_one_sb_msg(mgr, &mstb))
        goto out_clear_reply;
    /* a multi-packet message: keep it */
    if (!msg->have_eomt)
        return;
    if (!mgr->tx_busy || txmsg->dst != mstb) {
        RDBG(mgr->d, "mst: reply with no message %d %u %02x", mstb, msg->initial_hdr.lct, msg->msg[0]);
        goto out_clear_reply;
    }
    drm_dp_sideband_parse_reply(mgr, msg, &txmsg->reply);
    mgr->replies++;
    if (txmsg->reply.reply_type == RD_DP_SIDEBAND_REPLY_NAK) {
        mgr->naks++;
        RLOG(mgr->d, "mst: NAK reply: req 0x%02x, reason 0x%02x, nak data 0x%02x", txmsg->reply.req_type,
             txmsg->reply.nak.reason, txmsg->reply.nak.nak_data);
    }
    zero(msg, sizeof(*msg));
    txmsg->state = RD_SB_TX_RX;
    return;
out_clear_reply:
    zero(msg, sizeof(*msg));
}

/* drm_dp_mst_hpd_irq_handle_event. UP_REQ_EN is set as in DRM, but UP
 * requests (connection / resource status notifications) are not processed:
 * the topology is probed once (dp_mst_probe); a ready UP request is acked
 * and dropped. */
static int drm_dp_mst_hpd_irq_handle_event(rdna3_mst_mgr *mgr, const uint8_t *esi, uint8_t *ack, int *handled)
{
    uint8_t sc = (uint8_t)(esi[0] & 0x3f) | (uint8_t)((esi[0] & 0x80) >> 1);   /* DP_GET_SINK_COUNT */
    *handled = 0;
    if (sc != mgr->sink_count) {
        mgr->sink_count = sc;
        *handled = 1;
    }
    if (esi[1] & RD_DP_DOWN_REP_MSG_RDY) {
        drm_dp_mst_handle_down_rep(mgr);
        *handled = 1;
        ack[1] = (uint8_t)(ack[1] | RD_DP_DOWN_REP_MSG_RDY);
    }
    if (esi[1] & RD_DP_UP_REQ_MSG_RDY) {
        *handled = 1;
        ack[1] = (uint8_t)(ack[1] | RD_DP_UP_REQ_MSG_RDY);
    }
    return 0;
}

/* dm_handle_mst_sideband_msg_ready_event (DOWN_REP_MSG_RDY_EVENT): read
 * ESI, handle, ack, until no message is ready */
static void dm_handle_mst_sideband_msg_ready_event(rdna3_mst_mgr *mgr)
{
    uint8_t esi[4], ack[4];
    uint32_t dpcd_addr = RD_DPCD_SINK_COUNT_ESI, process_count = 0, k;
    int handled, retry;
    while (process_count < 30) {
        process_count++;
        if (dpcd_read(mgr, dpcd_addr, esi, sizeof(esi)) != (int)sizeof(esi))
            break;
        esi[1] &= RD_DP_DOWN_REP_MSG_RDY;
        if (!esi[1])
            break;
        for (k = 0; k < sizeof(ack); k++)
            ack[k] = 0;
        drm_dp_mst_hpd_irq_handle_event(mgr, esi, ack, &handled);
        if (handled) {
            /* ack at DPCD to notify downstream */
            for (retry = 0; retry < 3; retry++)
                if (dpcd_writeb(mgr, dpcd_addr + 1, ack[1]) == 1)
                    break;
            if (retry == 3)
                break;
        } else {
            break;
        }
    }
}

/* drm_dp_queue_down_tx + drm_dp_mst_wait_tx_reply: send every chunk, then
 * poll for the reply (poll_hpd_irq at once and every 50 ms, 4 s at most).
 * 1 when a reply came, RDNA3_EIO otherwise. */
static int drm_dp_mst_wait_tx_reply(rdna3_mst_mgr *mgr, rdna3_mst_sb_tx *txmsg)
{
    uint32_t waited = 0;
    int ret;
    mgr->msgs++;
    mgr->tx_busy = 1;
    zero(&mgr->down_rep_recv, sizeof(mgr->down_rep_recv));
    txmsg->state = RD_SB_TX_QUEUED;
    txmsg->cur_offset = 0;
    do {
        ret = process_single_tx_qlock(mgr, txmsg, 0);
    } while (ret == 0);
    if (ret < 0) {
        txmsg->state = RD_SB_TX_TIMEOUT;
        mgr->tx_busy = 0;
        return RDNA3_EIO;
    }
    for (;;) {
        dm_handle_mst_sideband_msg_ready_event(mgr);
        if (txmsg->state == RD_SB_TX_RX)
            break;
        if (waited >= 4000) {
            RLOG(mgr->d, "mst: timed out msg send, request 0x%02x", txmsg->msg[0]);
            mgr->timeouts++;
            mgr->tx_busy = 0;
            return RDNA3_EIO;
        }
        mgr->d->os.msleep(mgr->d->os.ctx, 50);
        waited += 50;
    }
    mgr->tx_busy = 0;
    return 1;
}

/* ---- topology ---- */

/* drm_dp_add_mst_branch_device */
static int drm_dp_add_mst_branch_device(rdna3_mst_mgr *mgr, uint8_t lct, const uint8_t *rad)
{
    int i;
    for (i = 0; i < RD_MST_MAX_BRANCHES; i++)
        if (!mgr->mstb[i].used)
            break;
    if (i == RD_MST_MAX_BRANCHES)
        return -1;
    zero(&mgr->mstb[i], sizeof(mgr->mstb[i]));
    mgr->mstb[i].used = 1;
    mgr->mstb[i].lct = lct;
    mgr->mstb[i].port_parent = -1;
    if (lct > 1)
        copy(mgr->mstb[i].rad, rad, lct / 2u);
    return i;
}

/* drm_dp_calculate_rad */
static uint8_t drm_dp_calculate_rad(const rdna3_mst_mgr *mgr, const rdna3_mst_port *port, uint8_t *rad)
{
    const rdna3_mst_branch *parent = &mgr->mstb[port->parent];
    int parent_lct = parent->lct, shift = 4, idx = (parent_lct - 1) / 2;
    if (parent_lct > 1) {
        copy(rad, parent->rad, (uint32_t)idx + 1);
        shift = (parent_lct % 2) ? 4 : 0;
    } else {
        rad[0] = 0;
    }
    rad[idx] = (uint8_t)(rad[idx] | (port->port_num << shift));
    return (uint8_t)(parent_lct + 1);
}

/* drm_dp_mst_is_end_device */
static int drm_dp_mst_is_end_device(uint8_t pdt, int mcs)
{
    switch (pdt) {
    case RD_DP_PEER_DEVICE_DP_LEGACY_CONV:
    case RD_DP_PEER_DEVICE_SST_SINK:
        return 1;
    case RD_DP_PEER_DEVICE_MST_BRANCHING:
        return !mcs;   /* an SST branch device */
    default:
        return 1;
    }
}

/* drm_dp_port_set_pdt: 1 when a branch was added under the port */
static int drm_dp_port_set_pdt(rdna3_mst_mgr *mgr, int p, uint8_t new_pdt, int new_mcs)
{
    rdna3_mst_port *port = &mgr->port[p];
    uint8_t rad[8] = { 0 }, lct;
    int mstb;
    if (port->pdt == new_pdt && port->mcs == new_mcs)
        return 0;
    if (port->pdt != RD_DP_PEER_DEVICE_NONE && !drm_dp_mst_is_end_device(port->pdt, port->mcs) && port->mstb >= 0) {
        mgr->mstb[port->mstb].used = 0;   /* drm_dp_mst_topology_put_mstb */
        port->mstb = -1;
    }
    port->pdt = new_pdt;
    port->mcs = new_mcs;
    if (port->pdt != RD_DP_PEER_DEVICE_NONE && !drm_dp_mst_is_end_device(port->pdt, port->mcs)) {
        lct = drm_dp_calculate_rad(mgr, port, rad);
        mstb = drm_dp_add_mst_branch_device(mgr, lct, rad);
        if (mstb < 0) {
            port->pdt = RD_DP_PEER_DEVICE_NONE;
            return RDNA3_ENOMEM;
        }
        port->mstb = mstb;
        mgr->mstb[mstb].port_parent = p;
        return 1;   /* send a link address to it */
    }
    return 0;
}

/* drm_dp_get_port */
static int drm_dp_get_port(const rdna3_mst_mgr *mgr, int mstb, uint8_t port_num)
{
    int i;
    for (i = 0; i < RD_MST_MAX_PORTS; i++)
        if (mgr->port[i].used && mgr->port[i].parent == mstb && mgr->port[i].port_num == port_num)
            return i;
    return -1;
}

/* drm_dp_mst_add_port */
static int drm_dp_mst_add_port(rdna3_mst_mgr *mgr, int mstb, uint8_t port_num)
{
    int i;
    for (i = 0; i < RD_MST_MAX_PORTS; i++)
        if (!mgr->port[i].used)
            break;
    if (i == RD_MST_MAX_PORTS)
        return -1;
    zero(&mgr->port[i], sizeof(mgr->port[i]));
    mgr->port[i].used = 1;
    mgr->port[i].parent = mstb;
    mgr->port[i].port_num = port_num;
    mgr->port[i].mstb = -1;
    mgr->port[i].vc_start_slot = -1;
    return i;
}

static int drm_dp_send_link_address(rdna3_mst_mgr *mgr, int mstb);

/* drm_dp_send_enum_path_resources: 1 when the port's PBN or FEC changed */
static int drm_dp_send_enum_path_resources(rdna3_mst_mgr *mgr, int mstb, int p)
{
    rdna3_mst_sb_tx *txmsg = &mgr->tx;
    rdna3_mst_port *port = &mgr->port[p];
    int ret;
    zero(txmsg, sizeof(*txmsg));
    txmsg->dst = mstb;
    build_enum_path_resources(txmsg, port->port_num);
    ret = drm_dp_mst_wait_tx_reply(mgr, txmsg);
    if (ret > 0) {
        ret = 0;
        if (txmsg->reply.reply_type == RD_DP_SIDEBAND_REPLY_NAK) {
            RDBG(mgr->d, "mst: enum path resources nak");
        } else {
            if (port->port_num != txmsg->reply.path_resources.port_number)
                RERR(mgr->d, "mst: incorrect port in the enum path resources reply");
            RLOG(mgr->d, "mst: enum path resources port %u: full PBN %u, available %u, FEC %d",
                 txmsg->reply.path_resources.port_number, txmsg->reply.path_resources.full_payload_bw_number,
                 txmsg->reply.path_resources.avail_payload_bw_number, txmsg->reply.path_resources.fec_capable);
            if (port->full_pbn != txmsg->reply.path_resources.full_payload_bw_number ||
                port->fec_capable != txmsg->reply.path_resources.fec_capable)
                ret = 1;
            port->full_pbn = txmsg->reply.path_resources.full_payload_bw_number;
            port->fec_capable = txmsg->reply.path_resources.fec_capable;
        }
    }
    return ret;
}

/* drm_dp_mst_handle_link_address_port: 1 changed, 0, or < 0 */
static int drm_dp_mst_handle_link_address_port(rdna3_mst_mgr *mgr, int mstb, const rdna3_mst_port_reply *pm)
{
    int p = drm_dp_get_port(mgr, mstb, pm->port_number), created = 0, changed = 0, send_link_addr = 0, ret, new_mcs;
    uint8_t new_pdt = RD_DP_PEER_DEVICE_NONE;
    rdna3_mst_port *port;
    if (p < 0) {
        p = drm_dp_mst_add_port(mgr, mstb, pm->port_number);
        if (p < 0)
            return RDNA3_ENOMEM;
        created = changed = 1;
    }
    port = &mgr->port[p];
    if (!created)
        changed = port->input != pm->input_port || port->ddps != pm->ddps ||
                  (port->ddps && (port->ldps != pm->legacy_device_plug_status || port->dpcd_rev != pm->dpcd_revision ||
                                  port->mcs != pm->mcs || port->pdt != pm->peer_device_type ||
                                  port->num_sdp_stream_sinks != pm->num_sdp_stream_sinks));
    port->input = pm->input_port;
    if (!port->input)
        new_pdt = pm->peer_device_type;
    new_mcs = pm->mcs;
    port->ddps = pm->ddps;
    port->ldps = pm->legacy_device_plug_status;
    port->dpcd_rev = pm->dpcd_revision;
    port->num_sdp_streams = pm->num_sdp_streams;
    port->num_sdp_stream_sinks = pm->num_sdp_stream_sinks;
    /* the PBN caps, on hotplug and on every re-probe */
    if (port->ddps && !port->input) {
        if (drm_dp_send_enum_path_resources(mgr, mstb, p) == 1)
            changed = 1;
    } else {
        port->full_pbn = 0;
    }
    ret = drm_dp_port_set_pdt(mgr, p, new_pdt, new_mcs);
    if (ret == 1)
        send_link_addr = 1;
    else if (ret < 0)
        return ret;
    if (!created && port->pdt == RD_DP_PEER_DEVICE_MST_BRANCHING && port->mcs)
        send_link_addr = 1;
    if (send_link_addr && port->mstb >= 0) {
        ret = drm_dp_send_link_address(mgr, port->mstb);
        if (ret == 1)
            changed = 1;
        else if (ret < 0)
            return ret;
    }
    return changed;
}

/* drm_dp_send_link_address: 1 when the branch's ports changed */
static int drm_dp_send_link_address(rdna3_mst_mgr *mgr, int mstb)
{
    rdna3_mst_sb_tx *txmsg = &mgr->tx;
    rdna3_mst_reply reply;
    uint32_t i, port_mask = 0;
    int ret, changed = 0;
    zero(txmsg, sizeof(*txmsg));
    txmsg->dst = mstb;
    build_link_address(txmsg);
    mgr->mstb[mstb].link_address_sent = 1;
    ret = drm_dp_mst_wait_tx_reply(mgr, txmsg);
    if (ret < 0) {
        RERR(mgr->d, "mst: sending link address failed (%d)", ret);
        goto out;
    }
    if (txmsg->reply.reply_type == RD_DP_SIDEBAND_REPLY_NAK) {
        RERR(mgr->d, "mst: link address NAK received");
        ret = RDNA3_EIO;
        goto out;
    }
    reply = txmsg->reply;   /* the ports below send their own messages */
    RLOG(mgr->d, "mst: link address reply (lct %u): %u ports", mgr->mstb[mstb].lct, reply.link_addr.nports);
    copy(mgr->mstb[mstb].guid, reply.link_addr.guid, 16);   /* drm_dp_check_mstb_guid: a GUID is kept as given */
    for (i = 0; i < reply.link_addr.nports && i < 16; i++) {
        const rdna3_mst_port_reply *pm = &reply.link_addr.ports[i];
        RLOG(mgr->d, "mst:   port %u: input %d, pdt %u, mcs %d, ddps %d, ldps %d, sdp %u/%u", pm->port_number,
             pm->input_port, pm->peer_device_type, pm->mcs, pm->ddps, pm->legacy_device_plug_status,
             pm->num_sdp_streams, pm->num_sdp_stream_sinks);
        port_mask |= 1u << pm->port_number;
        ret = drm_dp_mst_handle_link_address_port(mgr, mstb, pm);
        if (ret == 1)
            changed = 1;
        else if (ret < 0)
            goto out;
    }
    /* prune the ports this link address did not name */
    for (i = 0; i < RD_MST_MAX_PORTS; i++)
        if (mgr->port[i].used && mgr->port[i].parent == mstb && !(port_mask & (1u << mgr->port[i].port_num))) {
            if (mgr->port[i].mstb >= 0)
                mgr->mstb[mgr->port[i].mstb].used = 0;
            mgr->port[i].used = 0;
            changed = 1;
        }
    ret = 0;
out:
    if (ret < 0)
        mgr->mstb[mstb].link_address_sent = 0;
    return ret < 0 ? ret : changed;
}

/* drm_dp_send_clear_payload_id_table */
static void drm_dp_send_clear_payload_id_table(rdna3_mst_mgr *mgr, int mstb)
{
    rdna3_mst_sb_tx *txmsg = &mgr->tx;
    zero(txmsg, sizeof(*txmsg));
    txmsg->dst = mstb;
    build_clear_payload_id_table(txmsg);
    if (drm_dp_mst_wait_tx_reply(mgr, txmsg) > 0 && txmsg->reply.reply_type == RD_DP_SIDEBAND_REPLY_NAK)
        RDBG(mgr->d, "mst: clear payload table id nak received");
}

/* drm_dp_check_and_send_link_address */
static int drm_dp_check_and_send_link_address(rdna3_mst_mgr *mgr, int mstb)
{
    int ret, changed = 0, i;
    if (!mgr->mstb[mstb].link_address_sent) {
        ret = drm_dp_send_link_address(mgr, mstb);
        if (ret == 1)
            changed = 1;
        else if (ret < 0)
            return ret;
    }
    for (i = 0; i < RD_MST_MAX_PORTS; i++) {
        rdna3_mst_port *port = &mgr->port[i];
        if (!port->used || port->parent != mstb || port->input || !port->ddps || port->mstb < 0)
            continue;
        ret = drm_dp_check_and_send_link_address(mgr, port->mstb);
        if (ret == 1)
            changed = 1;
        else if (ret < 0)
            return ret;
    }
    return changed;
}

void dp_mst_mgr_init(rdna3_mst_mgr *mgr, rdna3_dev *d, uint32_t aux)
{
    zero(mgr, sizeof(*mgr));
    mgr->d = d;
    mgr->aux = aux;
    mgr->max_dpcd_transaction_bytes = 16;
    mgr->mst_primary = -1;
    dp_mst_update_slots(mgr, 0);
}

/* drm_dp_read_mst_cap */
int dp_mst_read_mst_cap(rdna3_dev *d, uint32_t aux, uint8_t rev)
{
    uint8_t mstm_cap = 0;
    if (rev < 0x12)
        return RD_DRM_DP_SST;
    if (dcaux_dpcd_read(d, aux, 0x021, &mstm_cap, 1) != 1)   /* DP_MSTM_CAP */
        return RD_DRM_DP_SST;
    if (mstm_cap & RD_DP_MST_CAP)
        return RD_DRM_DP_MST;
    if (mstm_cap & (1u << 1))                                 /* DP_SINGLE_STREAM_SIDEBAND_MSG */
        return RD_DRM_DP_SST_SIDEBAND_MSG;
    return RD_DRM_DP_SST;
}

int dp_mst_probe(rdna3_mst_mgr *mgr)
{
    int ret, i, n = 0;
    if (mgr->mst_primary < 0)
        return RDNA3_EIO;
    /* some branches report available PBN 0 until a CLEAR_PAYLOAD_ID_TABLE */
    if (!mgr->payload_id_table_cleared) {
        mgr->payload_id_table_cleared = 1;
        drm_dp_send_clear_payload_id_table(mgr, mgr->mst_primary);
    }
    ret = drm_dp_check_and_send_link_address(mgr, mgr->mst_primary);
    if (ret < 0)
        return RDNA3_EIO;
    for (i = 0; i < RD_MST_MAX_PORTS; i++)
        if (mgr->port[i].used)
            n++;
    return n;
}

int dp_mst_set_mst(rdna3_mst_mgr *mgr, int on)
{
    int i;
    if (!!on == mgr->mst_state)
        return RDNA3_OK;
    mgr->mst_state = !!on;
    if (on) {
        uint8_t dpcd[16] = { 0 };
        int primary;
        /* drm_dp_read_dpcd_caps (the base receiver caps) */
        if (dpcd_read(mgr, 0, dpcd, sizeof(dpcd)) != (int)sizeof(dpcd)) {
            mgr->mst_state = 0;
            return RDNA3_EIO;
        }
        copy(mgr->dpcd, dpcd, sizeof(dpcd));
        /* the initial branch device at LCT 1 */
        for (i = 0; i < RD_MST_MAX_BRANCHES; i++)
            mgr->mstb[i].used = 0;
        for (i = 0; i < RD_MST_MAX_PORTS; i++)
            mgr->port[i].used = 0;
        primary = drm_dp_add_mst_branch_device(mgr, 1, 0);
        mgr->mst_primary = primary;
        if (dpcd_writeb(mgr, 0x111, RD_DP_MST_EN | RD_DP_UP_REQ_EN | RD_DP_UPSTREAM_IS_SRC) != 1) {   /* DP_MSTM_CTRL */
            mgr->mst_state = 0;
            mgr->mst_primary = -1;
            return RDNA3_EIO;
        }
        /* the reset payload */
        dpcd_writeb(mgr, RD_DPCD_PAYLOAD_TABLE_UPDATE_STATUS, RD_DP_PAYLOAD_TABLE_UPDATED);
        {
            uint8_t reset[3] = { 0, 0, 0x3f };
            dpcd_write(mgr, RD_DPCD_PAYLOAD_ALLOCATE_SET, reset, 3);
        }
        mgr->payload_count = 0;
        mgr->payload_mask = 0;
        RLOG(mgr->d, "mst: AUX %u: MST on (DPCD %x.%x)", mgr->aux, dpcd[0] >> 4, dpcd[0] & 15);
        /* drm_dp_mst_queue_probe_work -> drm_dp_mst_link_probe_work, run here */
        return dp_mst_probe(mgr) < 0 ? RDNA3_EIO : RDNA3_OK;
    }
    /* off: MSTM_CTRL 0 (may fail with the device gone) */
    mgr->mst_primary = -1;
    dpcd_writeb(mgr, 0x111, 0);
    mgr->payload_id_table_cleared = 0;
    zero(&mgr->down_rep_recv, sizeof(mgr->down_rep_recv));
    for (i = 0; i < RD_MST_MAX_PORTS; i++)
        mgr->port[i].used = 0;
    for (i = 0; i < RD_MST_MAX_BRANCHES; i++)
        mgr->mstb[i].used = 0;
    RLOG(mgr->d, "mst: AUX %u: MST off", mgr->aux);
    return RDNA3_OK;
}

int dp_mst_first_sink_port(const rdna3_mst_mgr *mgr)
{
    int i;
    for (i = 0; i < RD_MST_MAX_PORTS; i++) {
        const rdna3_mst_port *p = &mgr->port[i];
        if (p->used && !p->input && p->ddps && p->pdt != RD_DP_PEER_DEVICE_NONE &&
            drm_dp_mst_is_end_device(p->pdt, p->mcs))
            return i;
    }
    return -1;
}

/* ---- remote access ---- */

/* drm_dp_send_dpcd_read */
int dp_mst_dpcd_read(rdna3_mst_mgr *mgr, int p, uint32_t offset, uint8_t *bytes, uint32_t size)
{
    rdna3_mst_sb_tx *txmsg = &mgr->tx;
    rdna3_mst_port *port;
    int ret;
    if (p < 0 || p >= RD_MST_MAX_PORTS || !mgr->port[p].used || size > 255)
        return RDNA3_EINVAL;
    port = &mgr->port[p];
    zero(txmsg, sizeof(*txmsg));
    build_dpcd_read(txmsg, port->port_num, offset, (uint8_t)size);
    txmsg->dst = port->parent;
    ret = drm_dp_mst_wait_tx_reply(mgr, txmsg);
    if (ret < 0)
        return ret;
    if (txmsg->reply.reply_type == 1) {
        RDBG(mgr->d, "mst: port %u: DPCD read at 0x%x (%u bytes) NAKed", port->port_num, offset, size);
        return RDNA3_EIO;
    }
    if (txmsg->reply.remote_read_ack.num_bytes != size)
        return RDNA3_EIO;   /* -EPROTO */
    copy(bytes, txmsg->reply.remote_read_ack.bytes, size);
    return (int)size;
}

/* drm_dp_mst_i2c_read: 'num' write transactions then one read (no STOP
 * between them: I2C_M_STOP is not set by drm_do_probe_ddc_edid) */
static int drm_dp_mst_i2c_read(rdna3_mst_mgr *mgr, int p, const uint8_t *w_addr, const uint8_t *const *w_buf,
                               const uint8_t *w_len, uint32_t nw, uint8_t r_addr, uint8_t *r_buf, uint8_t r_len)
{
    rdna3_mst_sb_tx *txmsg = &mgr->tx;
    rdna3_mst_port *port = &mgr->port[p];
    sb_req msg;
    uint32_t i;
    int ret;
    zero(&msg, sizeof(msg));
    msg.req_type = RD_DP_REMOTE_I2C_READ;
    msg.i2c_read.num_transactions = (uint8_t)nw;
    msg.port_number = port->port_num;
    for (i = 0; i < nw && i < 4; i++) {
        msg.i2c_read.t[i].i2c_dev_id = w_addr[i];
        msg.i2c_read.t[i].num_bytes = w_len[i];
        msg.i2c_read.t[i].bytes = w_buf[i];
        msg.i2c_read.t[i].no_stop_bit = 1;
    }
    msg.i2c_read.read_i2c_device_id = r_addr;
    msg.i2c_read.num_bytes_read = r_len;
    zero(txmsg, sizeof(*txmsg));
    txmsg->dst = port->parent;
    drm_dp_encode_sideband_req(&msg, txmsg);
    ret = drm_dp_mst_wait_tx_reply(mgr, txmsg);
    if (ret > 0) {
        if (txmsg->reply.reply_type == RD_DP_SIDEBAND_REPLY_NAK)
            return RDNA3_EIO;   /* -EREMOTEIO */
        if (txmsg->reply.remote_read_ack.num_bytes != r_len)
            return RDNA3_EIO;
        copy(r_buf, txmsg->reply.remote_read_ack.bytes, r_len);
        ret = (int)nw + 1;
    }
    return ret;
}

/* drm_do_probe_ddc_edid over drm_dp_mst_i2c_xfer: 'block' of the EDID */
static int drm_do_probe_ddc_edid(rdna3_mst_mgr *mgr, int p, uint32_t block, uint8_t *buf)
{
    uint8_t segment = (uint8_t)(block >> 1), start = (uint8_t)((block & 1) * 128);
    uint8_t addr[2], len[2] = { 1, 1 };
    const uint8_t *bufs[2];
    uint32_t n = 0;
    if (segment) {
        addr[n] = 0x30;          /* DDC_SEGMENT_ADDR */
        bufs[n++] = &segment;
    }
    addr[n] = 0x50;              /* DDC_ADDR */
    bufs[n++] = &start;
    return drm_dp_mst_i2c_read(mgr, p, addr, bufs, len, n, 0x50, buf, 128);
}

int dp_mst_read_edid(rdna3_mst_mgr *mgr, int p)
{
    rdna3_mst_port *port;
    uint32_t b, ext, sum, i, k;
    if (p < 0 || p >= RD_MST_MAX_PORTS || !mgr->port[p].used)
        return RDNA3_EINVAL;
    port = &mgr->port[p];
    port->edid_len = 0;
    /* drm_do_get_edid: the base block (4 tries for a good checksum), then the extensions */
    for (k = 0; k < 4; k++) {
        if (drm_do_probe_ddc_edid(mgr, p, 0, port->edid) < 0)
            return RDNA3_EIO;
        for (i = 0, sum = 0; i < 128; i++)
            sum += port->edid[i];
        if (!(sum & 0xff))
            break;
    }
    if (k == 4)
        return RDNA3_EIO;
    ext = port->edid[126];
    if (ext > RD_MST_EDID_MAX / 128 - 1)
        ext = RD_MST_EDID_MAX / 128 - 1;
    for (b = 1; b <= ext; b++)
        if (drm_do_probe_ddc_edid(mgr, p, b, port->edid + 128 * b) < 0)
            break;
    port->edid_len = 128 * b;
    RLOG(mgr->d, "mst: port %u: EDID %u bytes", port->port_num, port->edid_len);
    return (int)port->edid_len;
}

/* ---- payloads ---- */

/* drm_dp_mst_update_slots */
void dp_mst_update_slots(rdna3_mst_mgr *mgr, int is_128b132b)
{
    if (is_128b132b) {
        mgr->total_avail_slots = 64;
        mgr->start_slot = 0;
    } else {
        mgr->total_avail_slots = 63;
        mgr->start_slot = 1;
    }
}

/* drm_dp_link_symbol_cycles */
static int drm_dp_link_symbol_cycles(int lane_count, int pixels, int bpp_x16, int symbol_size, int is_mst)
{
    int cycles = (pixels * bpp_x16 + 16 * symbol_size * lane_count - 1) / (16 * symbol_size * lane_count);
    int align = is_mst ? 4 / lane_count : 1;
    return (cycles + align - 1) / align * align;
}

/* drm_dp_bw_overhead: 100 % + overhead in ppm (8b/10b, MST, SSC, no FEC, no DSC) */
static int drm_dp_bw_overhead(int lane_count, int hactive, int bpp_x16, int mst, int ssc)
{
    int symbol_size = 8, symbol_cycles;
    uint32_t overhead = 1000000;
    if (lane_count == 0 || hactive == 0 || bpp_x16 == 0)
        return 0;
    if (ssc)
        overhead += 6000;   /* SSC downspread and ref clock variation: 5300 + 300 ppm */
    symbol_cycles = drm_dp_link_symbol_cycles(lane_count, hactive, bpp_x16, symbol_size, mst);
    return (int)(((uint64_t)(uint32_t)(symbol_cycles * symbol_size * lane_count) * (overhead * 16) +
                  (uint64_t)(hactive * bpp_x16) - 1) / (uint64_t)(hactive * bpp_x16));
}

int drm_dp_calc_pbn_mode(int clock, int bpp)
{
    /* 4 lanes, 4096 active pixels assumed: no data padding, no DSC or FEC */
    int overhead = drm_dp_bw_overhead(4, 4096, bpp, 1, 1);
    uint64_t num = (uint64_t)(uint32_t)(clock * bpp) * (uint64_t)((64 * (uint32_t)overhead) >> 4);
    uint64_t den = 1000000ull * 8 * 54 * 1000;
    return (int)((num + den - 1) / den);
}

int dp_mst_find_time_slots(rdna3_mst_mgr *mgr, int p, int pbn)
{
    rdna3_mst_port *port;
    int req_slots, used = 0, i;
    if (p < 0 || p >= RD_MST_MAX_PORTS || !mgr->port[p].used || !mgr->pbn_div)
        return RDNA3_EINVAL;
    port = &mgr->port[p];
    /* DIV_ROUND_UP(dfixed_const(pbn), pbn_div.full): pbn_div is integral (dm_mst_get_pbn_divider) */
    req_slots = (int)(((uint32_t)pbn + mgr->pbn_div - 1) / mgr->pbn_div);
    /* drm_dp_mst_atomic_check_payload_alloc_limits */
    for (i = 0; i < RD_MST_MAX_PORTS; i++)
        if (i != p && mgr->port[i].used && mgr->port[i].vcpi)
            used += mgr->port[i].time_slots;
    if (used + req_slots > mgr->total_avail_slots)
        return RDNA3_EINVAL;
    if (!port->vcpi) {
        /* drm_dp_mst_atomic_check_mgr: VCPI = ffz(payload_mask) + 1 */
        for (i = 0; i < 31 && (mgr->payload_mask & (1u << i)); i++)
            ;
        mgr->payload_mask |= 1u << i;
        port->vcpi = i + 1;
        port->vc_start_slot = -1;
        port->payload_allocation_status = RD_PAYLOAD_ALLOCATION_NONE;
    }
    port->time_slots = req_slots;
    port->pbn = pbn;
    return req_slots;
}

void dp_mst_release_time_slots(rdna3_mst_mgr *mgr, int p)
{
    rdna3_mst_port *port;
    if (p < 0 || p >= RD_MST_MAX_PORTS)
        return;
    port = &mgr->port[p];
    if (port->vcpi)
        mgr->payload_mask &= ~(1u << (port->vcpi - 1));
    port->vcpi = 0;
    port->time_slots = 0;
    port->pbn = 0;
}

/* drm_dp_dpcd_write_payload: the VC payload ID table of the first branch */
static int drm_dp_dpcd_write_payload(rdna3_mst_mgr *mgr, int id, uint8_t start_slot, uint8_t num_slots)
{
    uint8_t payload_alloc[3], status = 0;
    int retries = 0;
    dpcd_writeb(mgr, RD_DPCD_PAYLOAD_TABLE_UPDATE_STATUS, RD_DP_PAYLOAD_TABLE_UPDATED);
    payload_alloc[0] = (uint8_t)id;
    payload_alloc[1] = start_slot;
    payload_alloc[2] = num_slots;
    if (dpcd_write(mgr, RD_DPCD_PAYLOAD_ALLOCATE_SET, payload_alloc, 3) != 3) {
        RDBG(mgr->d, "mst: failed to write payload allocation");
        return RDNA3_EIO;
    }
    for (;;) {
        if (dpcd_read(mgr, RD_DPCD_PAYLOAD_TABLE_UPDATE_STATUS, &status, 1) != 1)
            return RDNA3_EIO;
        if (status & RD_DP_PAYLOAD_TABLE_UPDATED)
            break;
        if (++retries >= 20) {
            RDBG(mgr->d, "mst: payload table status not set: %02x", status);
            return RDNA3_EINVAL;
        }
        mgr->d->os.msleep(mgr->d->os.ctx, 10);
    }
    return RDNA3_OK;
}

/* drm_dp_check_act_status: ACT handled within 3 s (200 us polls) */
int dp_mst_check_act_status(rdna3_mst_mgr *mgr)
{
    uint32_t waited_us = 0;
    uint8_t status = 0;
    for (;;) {
        /* do_get_act_status */
        if (dpcd_read(mgr, RD_DPCD_PAYLOAD_TABLE_UPDATE_STATUS, &status, 1) != 1) {
            RDBG(mgr->d, "mst: failed to read payload table status");
            return RDNA3_EIO;
        }
        if (status & RD_DP_PAYLOAD_ACT_HANDLED)
            return RDNA3_OK;
        if (waited_us >= 3000000) {
            RERR(mgr->d, "mst: failed to get ACT after 3000 ms, last status %02x", status);
            return RDNA3_ETIME;
        }
        mgr->d->os.udelay(mgr->d->os.ctx, 200);
        waited_us += 200;
    }
}

/* drm_dp_payload_send_msg: ALLOCATE_PAYLOAD to the port's branch */
static int drm_dp_payload_send_msg(rdna3_mst_mgr *mgr, int p, int id, int pbn)
{
    rdna3_mst_sb_tx *txmsg = &mgr->tx;
    rdna3_mst_port *port = &mgr->port[p];
    uint8_t sinks[16];
    uint32_t i;
    int ret;
    for (i = 0; i < port->num_sdp_streams && i < 16; i++)
        sinks[i] = (uint8_t)i;
    zero(txmsg, sizeof(*txmsg));
    txmsg->dst = port->parent;
    build_allocate_payload(txmsg, port->port_num, (uint8_t)id, (uint16_t)pbn, port->num_sdp_streams, sinks);
    ret = drm_dp_mst_wait_tx_reply(mgr, txmsg);
    if (ret > 0)
        ret = txmsg->reply.reply_type == RD_DP_SIDEBAND_REPLY_NAK ? RDNA3_EINVAL : RDNA3_OK;
    return ret;
}

/* drm_dp_create_payload_at_dfp */
static int drm_dp_create_payload_at_dfp(rdna3_mst_mgr *mgr, const rdna3_mst_port *port)
{
    return drm_dp_dpcd_write_payload(mgr, port->vcpi, (uint8_t)port->vc_start_slot, (uint8_t)port->time_slots);
}

/* drm_dp_create_payload_to_remote */
static int drm_dp_create_payload_to_remote(rdna3_mst_mgr *mgr, int p)
{
    return drm_dp_payload_send_msg(mgr, p, mgr->port[p].vcpi, mgr->port[p].pbn);
}

/* drm_dp_destroy_payload_at_remote_and_dfp (it is fine for these to fail) */
static void drm_dp_destroy_payload_at_remote_and_dfp(rdna3_mst_mgr *mgr, int p)
{
    rdna3_mst_port *port = &mgr->port[p];
    if (port->payload_allocation_status == RD_PAYLOAD_ALLOCATION_REMOTE) {
        drm_dp_payload_send_msg(mgr, p, port->vcpi, 0);
        port->payload_allocation_status = RD_PAYLOAD_ALLOCATION_DFP;
    }
    if (port->payload_allocation_status == RD_PAYLOAD_ALLOCATION_DFP)
        drm_dp_dpcd_write_payload(mgr, port->vcpi, (uint8_t)port->vc_start_slot, 0);
}

int dp_mst_add_payload_part1(rdna3_mst_mgr *mgr, int p)
{
    rdna3_mst_port *port;
    int ret;
    if (p < 0 || p >= RD_MST_MAX_PORTS || !mgr->port[p].used || !mgr->port[p].vcpi)
        return RDNA3_EIO;
    port = &mgr->port[p];
    if (mgr->payload_count == 0)
        mgr->next_start_slot = mgr->start_slot;
    port->vc_start_slot = mgr->next_start_slot;
    mgr->payload_count++;
    mgr->next_start_slot += port->time_slots;
    port->payload_allocation_status = RD_PAYLOAD_ALLOCATION_LOCAL;
    /* the payload at the immediate downstream facing port */
    ret = drm_dp_create_payload_at_dfp(mgr, port);
    if (ret < 0) {
        RDBG(mgr->d, "mst: failed to create the payload for port %u (%d)", port->port_num, ret);
        return ret;
    }
    port->payload_allocation_status = RD_PAYLOAD_ALLOCATION_DFP;
    return RDNA3_OK;
}

int dp_mst_add_payload_part2(rdna3_mst_mgr *mgr, int p)
{
    rdna3_mst_port *port = &mgr->port[p];
    int ret;
    if (port->payload_allocation_status != RD_PAYLOAD_ALLOCATION_DFP) {
        RDBG(mgr->d, "mst: part 1 of the payload for port %u failed, no part 2", port->port_num);
        return RDNA3_EIO;
    }
    ret = drm_dp_create_payload_to_remote(mgr, p);
    if (ret < 0)
        RERR(mgr->d, "mst: step 2 of the payload for port %u failed (%d)", port->port_num, ret);
    else
        port->payload_allocation_status = RD_PAYLOAD_ALLOCATION_REMOTE;
    return ret;
}

void dp_mst_remove_payload_part1(rdna3_mst_mgr *mgr, int p)
{
    /* the port is under the primary branch while it is in the table */
    if (p >= 0 && p < RD_MST_MAX_PORTS && mgr->port[p].used && mgr->mst_primary >= 0)
        drm_dp_destroy_payload_at_remote_and_dfp(mgr, p);
    if (p >= 0 && p < RD_MST_MAX_PORTS)
        mgr->port[p].payload_allocation_status = RD_PAYLOAD_ALLOCATION_LOCAL;
}

void dp_mst_remove_payload_part2(rdna3_mst_mgr *mgr, int p, int old_time_slots)
{
    rdna3_mst_port *port = &mgr->port[p];
    int i;
    /* the payloads after it move towards the start of the table */
    for (i = 0; i < RD_MST_MAX_PORTS; i++)
        if (i != p && mgr->port[i].used && mgr->port[i].vcpi && mgr->port[i].vc_start_slot > port->vc_start_slot)
            mgr->port[i].vc_start_slot -= old_time_slots;
    port->vc_start_slot = -1;
    mgr->payload_count--;
    mgr->next_start_slot -= old_time_slots;
    port->payload_allocation_status = RD_PAYLOAD_ALLOCATION_NONE;
}
