/*
 * amd_mqd.h - what the kernel hands the MQD init functions and the MES
 * scheduler for a user queue, shared by uapi/ (amdgpu_userq.c,
 * mes_userqueue.c) and hw/ (gfx_v11_0 / sdma_v6_0 MQDs, mes_v11_0):
 *
 *   amd_mqd_prop          struct amdgpu_mqd_prop (amdgpu.h)
 *   amd_mes_add_queue     struct mes_add_queue_input (amdgpu_mes.h)
 *   amd_mes_remove_queue  struct mes_remove_queue_input
 *
 * Field names and meanings as in Linux amdgpu (MIT); addresses are GPU
 * addresses: the MQD, context and fence ones in VMID 0 (MC), the queue,
 * rptr and wptr ones in the process VM.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMD_MQD_H
#define AMD_MQD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AMDGPU_GFX_PIPE_PRIO_* (amdgpu_gfx.h) and the HQD queue priority range */
#define AMD_GFX_PIPE_PRIO_NORMAL          1
#define AMD_GFX_PIPE_PRIO_HIGH            2
#define AMD_GFX_QUEUE_PRIORITY_MINIMUM    0
#define AMD_GFX_QUEUE_PRIORITY_MAXIMUM    15

/* enum amdgpu_mes_priority_level (amdgpu_mes.h) */
#define AMD_MES_PRIORITY_LEVEL_LOW        0
#define AMD_MES_PRIORITY_LEVEL_NORMAL     1
#define AMD_MES_PRIORITY_LEVEL_MEDIUM     2
#define AMD_MES_PRIORITY_LEVEL_HIGH       3
#define AMD_MES_PRIORITY_LEVEL_REALTIME   4

/* MQD sizes: struct v11_gfx_mqd / v11_compute_mqd / v11_sdma_mqd
 * (v11_structs.h), adev->mqds[ip].mqd_size */
#define AMD_V11_GFX_MQD_SIZE      2048
#define AMD_V11_COMPUTE_MQD_SIZE  2048
#define AMD_V11_SDMA_MQD_SIZE     512

/* the gfx user queue's firmware areas (gfx_v11_0_get_gfx_shadow_info_nocheck,
 * MQD_SHADOW_BASE_* / MQD_FWWORKAREA_*): bytes */
#define AMD_GFX11_SHADOW_SIZE           73728
#define AMD_GFX11_SHADOW_ALIGNMENT      256
#define AMD_GFX11_FWWORKAREA_SIZE       484
#define AMD_GFX11_FWWORKAREA_ALIGNMENT  256

typedef struct amd_mqd_prop {
    uint64_t mqd_gpu_addr;
    uint64_t hqd_base_gpu_addr;
    uint64_t rptr_gpu_addr;
    uint64_t wptr_gpu_addr;
    uint32_t queue_size;            /* bytes */
    uint32_t use_doorbell;
    uint32_t doorbell_index;
    uint64_t eop_gpu_addr;
    uint32_t hqd_pipe_priority;
    uint32_t hqd_queue_priority;
    uint32_t allow_tunneling;
    uint32_t hqd_active;
    uint64_t shadow_addr;
    uint64_t gds_bkup_addr;
    uint64_t csa_addr;
    uint64_t fence_address;
    uint32_t tmz_queue;
} amd_mqd_prop;

typedef struct amd_mes_add_queue {
    uint32_t process_id;
    uint64_t page_table_base_addr;
    uint64_t process_va_start;
    uint64_t process_va_end;
    uint64_t process_quantum;
    uint64_t process_context_addr;
    uint64_t gang_quantum;
    uint64_t gang_context_addr;
    uint32_t inprocess_gang_priority;      /* AMD_MES_PRIORITY_LEVEL_* */
    uint32_t gang_global_priority_level;
    uint32_t doorbell_offset;
    uint64_t mqd_addr;
    uint64_t wptr_addr;                    /* in the process VM */
    uint64_t wptr_mc_addr;                 /* the same through GART (VMID 0) */
    uint32_t queue_type;                   /* AMDGPU_HW_IP_GFX / COMPUTE / DMA */
    uint32_t paging;
    uint32_t gws_base, gws_size;
    uint64_t tba_addr, tma_addr;
    uint32_t trap_en;
    uint32_t skip_process_ctx_clear;
    uint32_t is_kfd_process;
    uint32_t is_aql_queue;
    uint32_t queue_size;                   /* dwords */
    uint32_t exclusively_scheduled;
} amd_mes_add_queue;

typedef struct amd_mes_remove_queue {
    uint32_t doorbell_offset;
    uint64_t gang_context_addr;
} amd_mes_remove_queue;

#ifdef __cplusplus
}
#endif
#endif
