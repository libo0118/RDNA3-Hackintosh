/*
 * dp_mst.h - DisplayPort MST topology manager: the part of the DRM DP MST
 * helper (drivers/gpu/drm/display/drm_dp_mst_topology.c, Linux v6.12, MIT)
 * that amdgpu's MST path uses, run synchronously over the AUX channel of
 * hw/dcaux.c:
 *
 *   sideband messages   drm_dp_msg_header_crc4, drm_dp_msg_data_crc4,
 *                       drm_dp_encode_sideband_msg_hdr, drm_dp_decode_sideband_msg_hdr,
 *                       drm_dp_encode_sideband_req, drm_dp_sideband_parse_reply and
 *                       its LINK_ADDRESS / ENUM_PATH_RESOURCES / ALLOCATE_PAYLOAD /
 *                       REMOTE_DPCD_READ / REMOTE_I2C_READ parsers,
 *                       build_link_address, build_enum_path_resources,
 *                       build_clear_payload_id_table, build_allocate_payload,
 *                       build_dpcd_read, set_hdr_from_dst_qlock, process_single_tx_qlock,
 *                       drm_dp_send_sideband_msg, drm_dp_get_one_sb_msg,
 *                       drm_dp_sideband_msg_set_header, drm_dp_sideband_append_payload,
 *                       drm_dp_mst_handle_down_rep, drm_dp_mst_hpd_irq_handle_event,
 *                       drm_dp_mst_wait_tx_reply (with amdgpu's poll_hpd_irq:
 *                       dm_handle_mst_sideband_msg_ready_event)
 *   topology            drm_dp_mst_topology_mgr_set_mst, drm_dp_mst_link_probe_work,
 *                       drm_dp_check_and_send_link_address, drm_dp_send_link_address,
 *                       drm_dp_mst_handle_link_address_port, drm_dp_port_set_pdt,
 *                       drm_dp_calculate_rad, drm_dp_mst_is_end_device,
 *                       drm_dp_add_mst_branch_device, drm_dp_get_mst_branch_device,
 *                       drm_dp_send_enum_path_resources, drm_dp_send_clear_payload_id_table,
 *                       drm_dp_read_mst_cap
 *   remote access       drm_dp_send_dpcd_read, drm_dp_mst_i2c_read (EDID as
 *                       drm_do_probe_ddc_edid asks for it)
 *   payloads            drm_dp_atomic_find_time_slots (the slot math), drm_dp_mst_update_slots,
 *                       drm_dp_add_payload_part1 / part2, drm_dp_remove_payload_part1 / part2,
 *                       drm_dp_create_payload_at_dfp, drm_dp_create_payload_to_remote,
 *                       drm_dp_destroy_payload_at_remote_and_dfp, drm_dp_payload_send_msg,
 *                       drm_dp_dpcd_write_payload, drm_dp_check_act_status,
 *                       drm_dp_calc_pbn_mode (drm_dp_bw_overhead, drm_dp_link_symbol_cycles)
 *
 * Fixed tables instead of DRM's kref'd lists; no UP requests (connection
 * status notifications: the topology is probed again instead), no DSC
 * passthrough, no QUERY_STREAM_ENC_STATUS.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RDNA3_DP_MST_H
#define RDNA3_DP_MST_H

#include "rdna3_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_MST_MAX_BRANCHES 8
#define RD_MST_MAX_PORTS    32
#define RD_MST_MSG_MAX      256
#define RD_MST_EDID_MAX     512

/* DPCD (drm_dp.h) */
#define RD_DPCD_SIDEBAND_MSG_DOWN_REQ_BASE  0x1000
#define RD_DPCD_SIDEBAND_MSG_UP_REP_BASE    0x1200
#define RD_DPCD_SIDEBAND_MSG_DOWN_REP_BASE  0x1400
#define RD_DPCD_SIDEBAND_MSG_UP_REQ_BASE    0x1600
#define RD_DPCD_PAYLOAD_ALLOCATE_SET        0x1c0
#define RD_DPCD_PAYLOAD_TABLE_UPDATE_STATUS 0x2c0
#define RD_DPCD_SINK_COUNT_ESI              0x2002
#define RD_DPCD_DEVICE_SERVICE_IRQ_VECTOR_ESI0 0x2003
#define RD_DP_PAYLOAD_TABLE_UPDATED         (1u << 0)
#define RD_DP_PAYLOAD_ACT_HANDLED           (1u << 1)
#define RD_DP_DOWN_REP_MSG_RDY              (1u << 4)
#define RD_DP_UP_REQ_MSG_RDY                (1u << 5)
#define RD_DP_MST_CAP                       (1u << 0)
#define RD_DP_MST_EN                        (1u << 0)
#define RD_DP_UP_REQ_EN                     (1u << 1)
#define RD_DP_UPSTREAM_IS_SRC               (1u << 2)
/* sideband request types */
#define RD_DP_LINK_ADDRESS                  0x01
#define RD_DP_ENUM_PATH_RESOURCES           0x10
#define RD_DP_ALLOCATE_PAYLOAD              0x11
#define RD_DP_CLEAR_PAYLOAD_ID_TABLE        0x14
#define RD_DP_REMOTE_DPCD_READ              0x20
#define RD_DP_REMOTE_I2C_READ               0x22
#define RD_DP_SIDEBAND_REPLY_NAK            0x01
/* peer device types */
#define RD_DP_PEER_DEVICE_NONE              0x0
#define RD_DP_PEER_DEVICE_SOURCE_OR_SST     0x1
#define RD_DP_PEER_DEVICE_MST_BRANCHING     0x2
#define RD_DP_PEER_DEVICE_SST_SINK          0x3
#define RD_DP_PEER_DEVICE_DP_LEGACY_CONV    0x4

/* enum drm_dp_mst_mode */
enum { RD_DRM_DP_SST = 0, RD_DRM_DP_MST, RD_DRM_DP_SST_SIDEBAND_MSG };
/* enum drm_dp_mst_payload_allocation */
enum { RD_PAYLOAD_ALLOCATION_NONE = 0, RD_PAYLOAD_ALLOCATION_LOCAL, RD_PAYLOAD_ALLOCATION_DFP,
       RD_PAYLOAD_ALLOCATION_REMOTE };

/* struct drm_dp_sideband_msg_hdr */
typedef struct rdna3_mst_sb_hdr {
    uint8_t lct, lcr, rad[8];
    int     broadcast, path_msg;
    uint8_t msg_len;
    int     somt, eomt;
    uint8_t seqno;
} rdna3_mst_sb_hdr;

/* struct drm_dp_sideband_msg_rx */
typedef struct rdna3_mst_sb_rx {
    uint8_t  chunk[48];
    uint8_t  msg[RD_MST_MSG_MAX];
    uint8_t  curchunk_len, curchunk_idx, curchunk_hdrlen;
    uint32_t curlen;
    int      have_somt, have_eomt;
    rdna3_mst_sb_hdr initial_hdr;
} rdna3_mst_sb_rx;

/* struct drm_dp_link_addr_reply_port */
typedef struct rdna3_mst_port_reply {
    int     input_port;
    uint8_t peer_device_type, port_number;
    int     mcs, ddps, legacy_device_plug_status;
    uint8_t dpcd_revision, peer_guid[16];
    uint8_t num_sdp_streams, num_sdp_stream_sinks;
} rdna3_mst_port_reply;

/* struct drm_dp_link_address_ack_reply */
typedef struct rdna3_mst_link_addr {
    uint8_t guid[16];
    uint8_t nports;
    rdna3_mst_port_reply ports[16];
} rdna3_mst_link_addr;

/* struct drm_dp_sideband_msg_reply_body (the replies this port parses) */
typedef struct rdna3_mst_reply {
    uint8_t reply_type, req_type;
    rdna3_mst_link_addr link_addr;
    struct { uint8_t guid[16]; uint8_t reason, nak_data; } nak;
    struct { uint8_t port_number; int fec_capable; uint16_t full_payload_bw_number, avail_payload_bw_number; } path_resources;
    struct { uint8_t port_number, vcpi; uint16_t allocated_pbn; } allocate_payload;
    struct { uint8_t port_number, num_bytes; uint8_t bytes[256]; } remote_read_ack;   /* DPCD and I2C read acks */
} rdna3_mst_reply;

/* struct drm_dp_sideband_msg_tx (one at a time) */
enum { RD_SB_TX_QUEUED = 0, RD_SB_TX_START_SEND, RD_SB_TX_SENT, RD_SB_TX_RX, RD_SB_TX_TIMEOUT };
typedef struct rdna3_mst_sb_tx {
    uint8_t  msg[RD_MST_MSG_MAX];
    uint32_t cur_offset, cur_len;
    int      path_msg;
    int      dst;                       /* branch index */
    int      state;
    rdna3_mst_reply reply;
} rdna3_mst_sb_tx;

/* struct drm_dp_mst_branch */
typedef struct rdna3_mst_branch {
    int     used;
    uint8_t lct, rad[8], guid[16];
    int     link_address_sent;
    int     port_parent;                /* port index, -1 for the primary */
} rdna3_mst_branch;

/* struct drm_dp_mst_port, with its drm_dp_mst_atomic_payload and EDID */
typedef struct rdna3_mst_port {
    int      used;
    int      parent;                    /* branch index */
    uint8_t  port_num;
    int      input;
    uint8_t  pdt;
    int      mcs, ddps, ldps;
    uint8_t  dpcd_rev;
    uint8_t  num_sdp_streams, num_sdp_stream_sinks;
    int      mstb;                      /* child branch, -1 */
    uint16_t full_pbn;
    int      fec_capable;
    /* payload */
    int      vcpi, time_slots, pbn, vc_start_slot, payload_allocation_status;
    /* the sink's EDID (drm_dp_mst_edid_read) */
    uint32_t edid_len;
    uint8_t  edid[RD_MST_EDID_MAX];
} rdna3_mst_port;

/* struct drm_dp_mst_topology_mgr + its drm_dp_mst_topology_state */
typedef struct rdna3_mst_mgr {
    rdna3_dev *d;
    uint32_t aux;
    int      mst_state;
    uint8_t  dpcd[16];
    uint32_t max_dpcd_transaction_bytes;
    int      payload_id_table_cleared;
    int      mst_primary;               /* branch index (0) or -1 */
    rdna3_mst_branch mstb[RD_MST_MAX_BRANCHES];
    rdna3_mst_port   port[RD_MST_MAX_PORTS];
    rdna3_mst_sb_rx  down_rep_recv;
    rdna3_mst_sb_tx  tx;
    int      tx_busy;
    /* the LINK_ADDRESS ack being walked, per branch: the walk recurses into
     * child branches, so it is kept here, not on the (kernel) stack */
    rdna3_mst_link_addr link_reply[RD_MST_MAX_BRANCHES];
    uint8_t  sink_count;
    /* drm_dp_mst_topology_state */
    int      start_slot, total_avail_slots;
    uint32_t pbn_div;                   /* integer part of the PBN per time slot */
    uint32_t payload_mask;
    int      payload_count, next_start_slot;
    /* counters (log, tests) */
    uint32_t msgs, replies, naks, timeouts, bad_replies;
} rdna3_mst_mgr;

/* drm_dp_mst_topology_mgr_init (the parts that are not locks / work items) */
void dp_mst_mgr_init(rdna3_mst_mgr *mgr, rdna3_dev *d, uint32_t aux);
/* drm_dp_read_mst_cap: RD_DRM_DP_* of the sink on 'aux' with DPCD revision 'rev' */
int  dp_mst_read_mst_cap(rdna3_dev *d, uint32_t aux, uint8_t rev);
/* drm_dp_mst_topology_mgr_set_mst; on: MSTM_CTRL, the payload table reset
 * and the probe (drm_dp_mst_link_probe_work, run here, not queued). 0 or
 * RDNA3_EIO. */
int  dp_mst_set_mst(rdna3_mst_mgr *mgr, int on);
/* drm_dp_mst_link_probe_work: CLEAR_PAYLOAD_ID_TABLE once, LINK_ADDRESS down
 * the tree, ENUM_PATH_RESOURCES for every connected output port. Returns
 * the number of ports, or RDNA3_EIO. */
int  dp_mst_probe(rdna3_mst_mgr *mgr);
/* drm_dp_mst_edid_read through drm_dp_mst_i2c_read (128-byte blocks, the
 * E-DDC segment for blocks 2-3). Bytes read or RDNA3_EIO. */
int  dp_mst_read_edid(rdna3_mst_mgr *mgr, int port);
/* drm_dp_send_dpcd_read on port 'port' (its parent branch reads the peer) */
int  dp_mst_dpcd_read(rdna3_mst_mgr *mgr, int port, uint32_t offset, uint8_t *buf, uint32_t size);
/* the first connected SST sink (end device) port with ddps, or -1 */
int  dp_mst_first_sink_port(const rdna3_mst_mgr *mgr);

/* drm_dp_mst_update_slots (8b/10b: 63 slots from slot 1) */
void dp_mst_update_slots(rdna3_mst_mgr *mgr, int is_128b132b);
/* drm_dp_calc_pbn_mode: 'clock' in kHz, bpp in .4 fixed point */
int  drm_dp_calc_pbn_mode(int clock, int bpp);
/* drm_dp_atomic_find_time_slots (pbn_div of the state; the VCPI assigned
 * as drm_dp_mst_atomic_check_mgr does: the lowest free). Slots or
 * RDNA3_EINVAL (more than the total available slots). */
int  dp_mst_find_time_slots(rdna3_mst_mgr *mgr, int port, int pbn);
/* drm_dp_atomic_release_time_slots */
void dp_mst_release_time_slots(rdna3_mst_mgr *mgr, int port);
/* drm_dp_add_payload_part1 / drm_dp_add_payload_part2 */
int  dp_mst_add_payload_part1(rdna3_mst_mgr *mgr, int port);
int  dp_mst_add_payload_part2(rdna3_mst_mgr *mgr, int port);
/* drm_dp_remove_payload_part1 / drm_dp_remove_payload_part2 (the old
 * payload's time slots as dm_helpers_construct_old_payload counts them) */
void dp_mst_remove_payload_part1(rdna3_mst_mgr *mgr, int port);
void dp_mst_remove_payload_part2(rdna3_mst_mgr *mgr, int port, int old_time_slots);
/* drm_dp_check_act_status: 0, RDNA3_ETIME, RDNA3_EIO */
int  dp_mst_check_act_status(rdna3_mst_mgr *mgr);

#ifdef __cplusplus
}
#endif
#endif
