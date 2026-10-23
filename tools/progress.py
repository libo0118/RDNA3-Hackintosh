#!/usr/bin/env python3
"""
progress.py - the porting status shown in README.md (docs/status/).

    python3 tools/progress.py            # regenerate docs/status/
    python3 tools/progress.py --check    # exit 1 when docs/status/ is stale

Two measures, each item in one of three states:
  works    - ported, and the ladder steps that exercise it passed on the
             card (docs/status/hw-verified.txt)
  ported   - in the code, checked only in the simulator / CI
  todo     - not ported yet

* amdgpu kernel functions: every function of the amdgpu source files the
  project ports from (tools/progress/sources.txt, index in
  tools/progress/amdgpu-index.tsv). A function is ported when the driver
  sources name it (ports carry the amdgpu function name in a comment).
* amdgpu UAPI: amdgpu ioctls, DRM core ioctls libdrm uses, INFO queries,
  firmware / sensor sub-queries and CS chunk ids (third_party/amd/
  amdgpu_drm.h, uapi/rdna3_drm.h). An item is implemented when
  uapi/rdna3_drm.c (libdrm_xnu for fds, the kext's ops.sensor for
  sensors) handles it instead of rejecting it; a firmware sub-query counts
  once the kext fills its version.

Items that do not apply - not on the RX 7800 XT, or of no use on macOS -
are left out of the totals and listed with their reason
(tools/progress/not-applicable.txt); one the driver ports still counts.
"""
import json
import re
import sys
import tempfile
from html import escape
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "status"
INDEX = ROOT / "tools" / "progress" / "amdgpu-index.tsv"
VERIFIED = OUT / "hw-verified.txt"
NOT_APPLICABLE = ROOT / "tools" / "progress" / "not-applicable.txt"
DRIVER_DIRS = ["hw", "kext", "uapi", "common", "fb", "accel", "userspace/libdrm_xnu"]
DRIVER_EXT = {".c", ".h", ".cpp", ".hpp", ".m", ".mm"}

# group -> ladder steps (docs/TESTING-VM.md) that must pass on the card for
# the group to count as working; a step no ladder has yet keeps it unverified
AMDGPU_STEPS = {
    "PSP 13": ["4"], "SMU 13 power": ["5"], "GMC / GPUVM": ["6", "7"],
    "IH interrupts": ["6", "6b"], "SoC / NBIO / HDP": ["1", "4"], "SDMA 6": ["7"],
    "GFX 11": ["8"], "Rings / fences": ["7", "8"], "MES queues": ["mes"],
    "User queues": ["userq"],
    "Discovery / VBIOS": ["2"], "DMCUB": ["5b"], "Display: OTG / AUX": ["2", "9"],
    "Display: modeset": ["modeset"],
    "Display: DP link": ["dplink"],
    "Display: DSC": ["dsc"],
    "Display: MST": ["mst"],
    "Display: UHBR": ["uhbr"],
    "Display: ODM": ["odm"],
    "Display: hotplug": ["hotplug"],
    "Display: interrupts": ["vblirq"],
    "Display: color": ["gamma"],
    "Display: audio": ["audio"],
    "VCN / JPEG video": ["vcn"],
}
UAPI_STEPS = {
    "amdgpu ioctls": ["7", "8"], "DRM core ioctls": ["7", "8"], "INFO queries": ["7"],
    "INFO firmware / sensors": ["5", "7"], "CS chunks": ["7", "8"],
}
# items added to a group after its steps passed: they need their own step
# (regex over the item name -> steps) before they count as working
ITEM_STEPS = [
    (re.compile(r"DRM_AMDGPU_USERQ(_SIGNAL|_WAIT)?|AMDGPU_INFO_UQ_FW_AREAS"), ["userq"]),
    # the video block's parts of verified groups (SMU power messages, the
    # NBIO doorbell range, the codec tables, the generic ring helpers the
    # VCN / JPEG rings use)
    (re.compile(r"smu_v13_0_set_(vcn|jpeg)_enable|nbio_v4_3_vcn_doorbell_range|soc21_query_video_codecs"
                r"|amdgpu_fence_count_emitted|amdgpu_ib_ring_tests|amdgpu_ring_commit"
                r"|amdgpu_ring_emit_reg_write_reg_wait_helper|amdgpu_ring_insert_nop"), ["vcn"]),
    # VCN dynamic power gating (rdna3vcn=2) and the video block across a
    # system sleep (rdna3vcn=2 + rdna3resume=1)
    (re.compile(r"vcn_v4_0_(start_dpg_mode|stop_dpg_mode|pause_dpg_mode|mc_resume_dpg_mode"
                r"|disable_clock_gating_dpg_mode|enable_ras)|amdgpu_vcn_psp_update_sram"), ["vcn2"]),
    (re.compile(r"(vcn_v4_0|jpeg_v4_0)_(suspend|resume)|amdgpu_(vcn|jpeg)_(suspend|resume)"), ["vcnsleep"]),
    # amdgpu enables CP gfx shadowing only on SR-IOV VFs: no step can run it
    (re.compile(r"AMDGPU_CHUNK_ID_CP_GFX_SHADOW|gfx_v11_0_ring_emit_(gfx_shadow|init_cond_exec)"
                r"|amdgpu_ring_patch_cond_exec|gfx_v11_0_get_gfx_shadow_info"), ["gfxshadow"]),
    (re.compile(r"gfx_v11_0_gfx_mqd_set_priority|sdma_v6_0_mqd_init"
                r"|gfx_v11_0_get_gfx_shadow_info_nocheck"), ["userq"]),
    # the DP side of files the TMDS modeset step verified
    (re.compile(r"dcn10_link_encoder_(dp_set_lane_settings|dp_set_phy_pattern|enable_dp_output"
                r"|get_max_link_cap|set_dp_phy_pattern_training_pattern)"
                r"|dcn32_(disable_link_output|unblank_stream)|(enable|disable)_link_dp|enable_link|disable_prbs_mode"
                r"|enable_phy_bypass_mode|enc1_configure_encoder|enc32_(dp_set_odm_combine|enable_fifo"
                r"|reset_fifo|set_dig_input_mode|stream_encoder_dp_unblank)|link_encoder_disable"
                r"|link_transmitter_control|set_dp_phy_pattern_passthrough_mode"
                r"|set_link_training_complete|setup_panel_mode|get_frontend_source|dce_aux_configure_timeout|enable_stream_features"), ["dplink"]),
    # FEC and DSC (rdna3dsc) in files of the modeset / DP link groups
    (re.compile(r"dcn32_dsc_pg_(control|status)|dcn32_update_dsc_pg|enc32_dp_set_dsc_config|dp_set_dsc_on_rx"
                r"|link_set_dsc_(enable|on_stream|pps_packet)|dp_is_fec_supported|dp_should_enable_fec"
                r"|dp_set_fec_(enable|ready)|get_active_converter_info"), ["dsc"]),
    # DP MST (rdna3mst) in files of the modeset / DP link groups
    (re.compile(r"(de)?allocate_mst_payload|enable_link_dp_mst|enable_mst_on_sink|get_pbn_(from_bw_in_kbps"
                r"|from_timing|per_slot)|log_vcp_x_y|remove_stream_from_alloc_table|update_mst_stream_alloc_table"
                r"|dcn10_link_encoder_(enable_dp_mst_output|update_mst_stream_allocation_table)"
                r"|fill_stream_allocation_row_info|decide_mst_link_settings|read_is_mst_supported"
                r"|set_dio_throttled_vcp_size|update_dio_stream_allocation_table"
                r"|enc1_stream_encoder_set_throttled_vcp_size"), ["mst"]),
    # stage 4 item K, Display Core pieces in verified groups: the OTG
    # read-back of step otg (rdna3otg=1), amdgpu's DMCUB reset and its
    # diagnostics in step dmubreset (rdna3dmubreset=1)
    (re.compile(r"optc1_(get_position|is_counter_moving|get_crtc_scanoutpos|get_otg_active_size|wait_for_state"
                r"|setup_vertical_interrupt0|is_optc_underflow_occurred)"
                r"|optc32_(get_double_buffer_pending|get_odm_combine_segments)"), ["otg"]),
    (re.compile(r"dmub_dcn32_(reset|set_gpint|is_gpint_acked|get_gpint_response|get_diagnostic_data)"
                r"|dmub_srv_(get_diagnostic_data|get_outbox0_msg)"), ["dmubreset"]),
    # DC's HDMI stream (rdna3hdmi=1, step hdmi): stream attributes, AV mute,
    # infoframes, SCDC, the link encoder's output validation, the EDID caps
    (re.compile(r"enc32_stream_encoder_(hdmi|dvi)_set_stream_attribute|enc1_stream_encoder_set_stream_attribute_helper"
                r"|enc1_stream_encoder_set_avmute|enc1_reset_hdmi_stream_attribute|set_avmute"
                r"|enc3_(update_hdmi_info_packet|stream_encoder_update_hdmi_info_packets|stream_encoder_stop_hdmi_info_packets)"
                r"|dcn10_link_encoder_validate_(output_with_stream|dvi_output|hdmi_output|dp_output)"
                r"|get_tmds_output_pixel_clock_100hz|dm_helpers_parse_edid_caps|apply_edid_quirks|edid_extract_panel_id"),
     ["hdmi"]),
    # the cursor image through MALL (rdna3cursormall=1, step cursormall)
    (re.compile(r"hubp32_cursor_set_attributes"), ["cursormall"]),
    # DC's sequence extras around a mode switch (rdna3dcseq=1, step dcseq)
    (re.compile(r"apply_symclk_on_tx_off_wa|dcn32_resync_fifo_dccg_dio|dccg32_trigger_dio_fifo_resync"), ["dcseq"]),
    # the hard-min status wait with DC's delay accounting (the DISPCLK raise of step modeset2)
    (re.compile(r"dcn32_smu_(send_msg_with_param_delay|wait_for_response_delay|get_hard_min_status)"), ["modeset2"]),
    # DP 2.1 UHBR (rdna3uhbr, step uhbr): the 128b/132b parts of the
    # modeset / DP link / DSC groups (the DCCG's DTBCLK / DPSTREAMCLK, the
    # pixel rate divider, the OTG's HPO output, the SST payload, the UHBR
    # caps, cable id, non-transparent LTTPR mode, the max-bandwidth fallback,
    # the 128b/132b stream overhead)
    (re.compile(r"dccg32_set_(dpstreamclk|dtbclk_dto|dtbclk_p_src)|dcn32_calculate_pix_rate_divider"
                r"|dcn32_calculate_dccg_k1_k2_values|optc3_set_out_mux"
                r"|get_lane_status|link_calculate_sst_avg_time_slots_per_mtp|update_sst_payload"
                r"|write_128b_132b_sst_payload_allocation_table|poll_for_allocation_change_trigger"
                r"|configure_lttpr_mode_non_transparent|decide_fallback_link_setting_max_bw_policy"
                r"|dp_is_128b_132b_signal|dpcd_write_cable_id_to_dprx|get_cable_max_link_rate|retrieve_cable_id"
                r"|is_immediate_downstream|apply_128b_132b_stream_overhead"), ["uhbr"]),
    # ODM combine (rdna3odm, step odm) in the modeset / OTG groups
    (re.compile(r"dcn32_update_odm|get_odm_config|dcn32_update_dsc_on_stream|optc32_set_odm_(combine|bypass)"),
     ["odm"]),
    # display audio (rdna3audio, step audio): the stream encoders' AFMT / DP
    # SDP / HPO audio, the link hwss audio hooks, the audio DTBCLK DTO, the
    # PMFW's PME workaround, in the DP link / DSC / UHBR / modeset groups
    (re.compile(r"enc1_se_\w+|enc3_(se_\w+|audio_mute_control)|get_audio_clock_info|speakers_to_channels"
                r"|(setup|enable|disable)_(dio|hpo_dp)_audio_(output|packet)|dcn31_hpo_dp_stream_enc_audio_\w+"
                r"|hpo_dp_is_gsp_enabled|dccg31_set_audio_dtbclk_dto|dcn32_enable_pme_wa|dcn32_smu_set_pme_workaround"),
     ["audio"]),
    # output pixel formats (rdna3fmt, step fmt): the OPP's formatter and
    # pattern generator
    (re.compile(r"opp1_(program_fmt|program_bit_depth_reduction|set_truncation|set_spatial_dither|set_clamping"
                r"|set_dyn_expansion|set_pixel_encoding|program_clamping_and_pixel_encoding)"
                r"|opp2_(set_disp_pattern_generator|program_dpg_dimensions|dpg_set_blank_color|dpg_is_blanked"
                r"|dpg_is_pending)"), ["fmt"]),
    # display power (rdna3mall, step mall, item O): the PMFW's watermark
    # range table, DC's p-state / FCLK / CAB messages, the watermark sets,
    # MALL_SEL, the forced p-state disallow, the idle CAB, dcn32_hubbub.c
    (re.compile(r"dcn32_(build_wm_range_table|notify_wm_ranges|smu_transfer_wm_table_dram_2_smu"
                r"|calculate_cab_allocation|check_no_memory_request_for_cab|apply_idle_power_optimizations"
                r"|smu_send_cab_for_uclk_message|update_mall_sel|smu_send_fclk_pstate_message|update_force_pstate"
                r"|prepare_bandwidth|initialize_min_clocks|update_clocks_update_dtb_dto|clock_read_ss_info"
                r"|interdependent_update_lock|program_outstanding_updates|get_memclk_states_from_smu"
                r"|set_hard_max_memclk|init_crb|program_compbuf_size)"
                r"|hubp32_(update_mall_sel|update_force_pstate_disallow|update_force_cursor_pstate_disallow)"
                r"|hubbub32_(program_watermarks|program_(urgent|stutter|pstate|usr)_watermarks|init_watermarks"
                r"|wm_read_state|force_wm_propagate_to_pipes|force_usr_retraining_allow|get_mall_en|set_request_limit)"
                r"|convert_and_clamp"), ["mall"]),
    # SubVP (rdna3subvp, step subvp): the phantom pipe, its OTG / HUBP /
    # OPP pattern generator, the DMCUB's SubVP command, lock and flip
    # address, the phantom plane's input color
    (re.compile(r"dcn32_(enable_phantom_streams|disable_phantom_streams|blank_phantom|init_blank|commit_subvp_config"
                r"|subvp_pipe_control_lock|subvp_pipe_control_lock_fast|apply_update_flags_for_phantom"
                r"|is_pipe_topology_transition_seamless|program_mall_pipe_config|set_input_transfer_func"
                r"|set_mcm_luts|set_mpc_shaper_3dlut)"
                r"|hubp32_(phantom_hubp_post_enable|prepare_subvp_buffering)"
                r"|optc32_(disable_phantom_otg|phantom_crtc_post_enable)"
                r"|dmub_dcn32_save_surf_addr|dmub_srv_subvp_save_surf_addr|is_subvp_phantom_topology_transition_seamless"
                r"|opp2_(set_disp_pattern_generator|dpg_set_blank_color|dpg_is_blanked|dpg_is_pending"
                r"|program_dpg_dimensions)"), ["subvp"]),
    # item O, ported and simulated, nothing in the kext reaches it: the
    # phantom viewport follows a moving plane (macOS planes start at 0)
    (re.compile(r"dcn32_update_phantom_vp_position"), ["none"]),
    # ported and checked in the simulator, but no ladder step drives them
    # yet (nothing in the kext calls them): they stay "ported"
    (re.compile(r"optc1_(setup_vertical_interrupt[12]|validate_timing|is_two_pixels_per_container"
                r"|enable_optc_clock|set_static_screen_control|enable_reset_trigger|enable_crtc_reset"
                r"|did_triggered_reset_occur|disable_reset_trigger|clear_optc_underflow)"
                r"|optc3_(tg_init|set_timing_double_buffer|lock_doublebuffer_enable|lock_doublebuffer_disable"
                r"|triplebuffer_lock|set_drr_trigger_window|set_vtotal_change_limit|set_vtotal_min_max)"
                r"|optc32_(set_drr|setup_manual_trigger|set_h_timing_div_manual_mode)|is_two_pixels_per_containter"
                r"|dmub_dcn32_(get_gpint_dataout|send_inbox0_cmd|clear_inbox0_ack_register|read_inbox0_ack_register"
                r"|get_current_time)"
                r"|dmub_srv_(send_gpint_command|get_gpint_response|get_gpint_dataout|send_inbox0_cmd"
                r"|clear_inbox0_ack|wait_for_inbox0_ack|set_power_state|get_fw_boot_status)"
                r"|dcn32_(is_dp_dig_pixel_rate_div_policy|enable_power_gating_plane"
                r"|are_clock_states_equal|get_dispclk_from_dentist)|hubp32_init|is_h_timing_divisible_by_2"), ["none"]),
    # stage 4 item J: amdgpu's own sequence where the verified profile
    # differs, behind rdna3amdgpu=1 (ladder step amdgpu), and the ports
    # that came with it into groups the card verified
    (re.compile(r"acquire_psp_cmd_buf|release_psp_cmd_buf|psp_(asd|hdcp|dtm)_(initialize|invoke|terminate)"
                r"|psp_check_pmfw_centralized_cstate_management|psp_early_init|psp_free_shared_bufs"
                r"|psp_hw_fini|psp_suspend|psp_rl_load|psp_prep_ta_(invoke|load|unload)_cmd_buf"
                r"|psp_prep_tmr_unload_cmd_buf|psp_ta_(free_shared_buf|init_shared_buf|invoke|load|unload)"
                r"|psp_tmr_(terminate|unload)|psp_v13_0_ring_destroy"), ["amdgpu"]),
    (re.compile(r"smu_v13_0_0_(get_allowed_feature_mask|is_mode1_reset_supported|set_mp1_state)"
                r"|smu_v13_0_(ack_ac_dc_interrupt|allow_ih_interrupt|deep_sleep_control|disable_pmfw_state"
                r"|enable_thermal_alert|disable_thermal_alert|irq_process|notify_display_change"
                r"|process_pending_interrupt|register_irq_handler|set_irq_state|set_performance_level"
                r"|update_pcie_parameters|gfx_ulv_control|gpo_control|set_power_source)"
                r"|smu_(enable_umd_pstate|force_performance_level|adjust_power_state_dynamic"
                r"|swctf_delayed_work_handler|reset_mp1_state)"
                r"|soc21_update_umd_stable_pstate|gfx_v11_0_update_perf_clk"), ["amdgpu"]),
    (re.compile(r"amdgpu_vm_pt_(add_list|alloc|ancestor|clear|continue_dfs|create|descendant|entries_mask"
                r"|first_dfs|free_list|next|next_dfs|num_entries|parent|sibling|size|start)"
                r"|amdgpu_vm_pte_(fragment|update_flags|update_noretry_flags)|amdgpu_vm_ptes_update"
                r"|amdgpu_gmc_set_pte_pde|amdgpu_gmc_allocate_vm_inv_eng|gmc_v11_0_late_init"
                r"|gfxhub_v3_0_3_(gart_disable|get_mc_fb_offset)|gmc_v11_0_(gart_disable|hw_fini)"), ["amdgpu"]),
    (re.compile(r"ih_v6_0_(doorbell_rptr|init_register_offset|rb_cntl|self_irq|sw_init)|amdgpu_ih_ring_fini"
                r"|nbio_v4_3_ih_doorbell_range|sdma_v6_0_(ctxempty_int_enable|hw_fini|suspend)"), ["amdgpu"]),
    (re.compile(r"gfx_v11_0_init_compute_vmid|soc21_read_register|soc21_get_register_value"
                r"|amdgpu_discovery_init|amdgpu_discovery_validate_ip"), ["amdgpu"]),
    # GFX 11 with rdna3amdgpu=1: queue topology, padded commits, pipeline
    # sync, CP fault interrupts, IB tests, the soft reset's index mutex
    (re.compile(r"amdgpu_gfx_(bit_to_me_queue|get_num_kcq|graphics_queue_acquire|is_compute_multipipe_capable"
                r"|is_graphics_multipipe_capable|is_high_priority_compute_queue|is_high_priority_graphics_queue"
                r"|is_me_queue_enabled|is_mec_queue_enabled|me_queue_to_bit|mec_queue_to_bit)"
                r"|amdgpu_queue_mask_bit_to_mec_queue|gfx_v11_ring_insert_nop"
                r"|gfx_v11_0_(bad_op_irq|early_init|get_tcc_info|gfx_ring_init|gpu_early_init|handle_priv_fault"
                r"|late_init|me_init|mec_init|priv_inst_irq|priv_reg_irq|request_gfx_index_mutex"
                r"|ring_emit_fence_kiq|ring_emit_pipeline_sync|ring_get_wptr_compute|ring_get_wptr_gfx"
                r"|ring_test_ib|select_me_pipe_q|set_bad_op_fault_state|set_priv_inst_fault_state"
                r"|set_priv_reg_fault_state|suspend|sw_init|update_spm_vmid|wait_for_idle)"), ["amdgpu"]),
    # GMC / IH / PSP with rdna3amdgpu=1: TLB flushes through the KIQ, PASID
    # mappings, the stolen pre-OS framebuffer, suspend before a full reset
    (re.compile(r"amdgpu_gmc_(flush_gpu_tlb|fw_reg_write_reg_wait|get_vbios_allocations)"
                r"|gmc_v11_0_(emit_pasid_mapping|get_vbios_fb_size|suspend)|ih_v6_0_suspend"), ["amdgpu"]),
    # rings / fences with rdna3amdgpu=1: the fences drained at unload, the
    # interrupt off while fences are forced, the MES ring-full check
    (re.compile(r"amdgpu_fence_driver_(hw_fini|isr_toggle)|amdgpu_fence_wait_empty|amdgpu_ring_undo"), ["amdgpu"]),
    # MES with rdna3amdgpu=1 (and rdna3mes=1): misc ops, legacy queue frames,
    # gang suspend / resume, the queue resets
    (re.compile(r"mes_v11_0_(misc_op|map_legacy_queue|unmap_legacy_queue|reset_legacy_queue|suspend_gang"
                r"|resume_gang|suspend|resume|ring_get_rptr|ring_get_wptr)|amdgpu_gfx_mqd_sw_init"
                r"|gfx_v11_0_reset_(kcq|kgq)"), ["amdgpu"]),
    # SDMA 6 with rdna3amdgpu=1: amdgpu_ib_schedule on the DMA ring (padded,
    # burst NOPs, COND_EXE, HDP flush, VM flush on its own engine), the IB
    # test, amdgpu_vm_sdma's PTE packets
    (re.compile(r"sdma_v6_0_(ring_emit_hdp_flush|ring_emit_mem_sync|ring_emit_pipeline_sync|ring_emit_reg_wait"
                r"|ring_emit_reg_write_reg_wait|ring_emit_vm_flush|ring_emit_wreg|ring_get_rptr|ring_get_wptr"
                r"|ring_init_cond_exec|ring_pad_ib|ring_test_ib|vm_copy_pte|vm_set_pte_pde|vm_write_pte"
                r"|is_idle|wait_for_idle)"), ["amdgpu"]),
    # Discovery / VBIOS with rdna3amdgpu=1: VBIOS clocks / VRAM / scratch,
    # firmware validation and names, the harvest / MALL / VCN tables
    (re.compile(r"amdgpu_atomfirmware_(allocate_fb_scratch|allocate_fb_v2_1|allocate_fb_v2_2|get_clock_info"
                r"|get_vram_info|dynamic_boot_config_supported|mem_training_supported)"
                r"|convert_atom_mem_type_to_vram_type|amdgpu_ucode_(validate|request|ip_version_decode|hdr_version|name)"
                r"|amdgpu_discovery_(harvest_ip|read_from_harvest_table|read_harvest_bit_per_ip|harvest_config_quirk"
                r"|get_harvest_info|get_mall_info|get_vcn_info)"), ["amdgpu"]),
    # SoC / NBIO with rdna3amdgpu=1: ASPM / LTR, init_registers, BIF
    # clock gating, the doorbell apertures at late_init / hw_fini, resets
    # on init / resume
    (re.compile(r"nbio_v4_3_(init_registers|program_aspm|program_ltr|update_medium_grain_clock_gating"
                r"|update_medium_grain_light_sleep)|soc21_(program_aspm|common_hw_fini|common_late_init"
                r"|common_resume|common_suspend|need_reset_on_init|need_reset_on_resume)"
                r"|amdgpu_device_should_use_aspm"), ["amdgpu"]),
    # SMU 13 PM control (rdna3_pmctl) joined the verified step 5 group
    (re.compile(r"smu_v13_0_baco_\w+|smu_v13_0_get_bamaco_support"), ["baco"]),
    (re.compile(r"smu_v13_0_(get|set)_fan_control_mode|smu_v13_0_auto_fan_control"
                r"|smu_v13_0_set_fan_(static_mode|speed_pwm|speed_rpm)"
                r"|smu_v13_0_0_(get_fan_speed_pwm|get_power_limit|set_power_limit"
                r"|get_overdrive_table|upload_overdrive_table|set_default_od_settings"
                r"|get_od_setting_limits|is_od_feature_supported|od_edit_dpm_table"
                r"|od_restore_table_single|restore_user_od_settings|set_supported_od_feature_mask"
                r"|force_clk_levels|get_dpm_ultimate_freq|populate_umd_state_clk"
                r"|get_power_profile_mode|get_thermal_temperature_range|get_unique_id"
                r"|get_current_clk_freq_by_table"
                r"|set_df_cstate)"), ["pmctl"]),
]

GREEN, YELLOW, GRAY = "#2ea043", "#d29922", "#6e7681"
BG, TEXT = "#0d1117", "#ffffff"
PANEL_W, GAP, MAP_H, HEAD = 495, 10, 300, 52


# ------------------------------------------------------------------ data

def verified_steps():
    steps = set()
    if VERIFIED.exists():
        for line in VERIFIED.read_text().splitlines():
            line = line.split("#", 1)[0].split()
            if line:
                steps.add(line[0])
    return steps


def driver_words():
    words = set()
    for d in DRIVER_DIRS:
        for p in (ROOT / d).rglob("*"):
            if p.suffix in DRIVER_EXT and p.is_file():
                words.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", p.read_text(errors="ignore")))
    return words


def not_applicable():
    """[(compiled regex, reason)] from tools/progress/not-applicable.txt"""
    rules = []
    if NOT_APPLICABLE.exists():
        for line in NOT_APPLICABLE.read_text().splitlines():
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            pat, _, why = re.split(r"(\s{2,})", line.strip(), maxsplit=1)
            rules.append((re.compile(pat), why.strip()))
    return rules


NA_RULES = not_applicable()


def na_reason(item):
    for rx, why in NA_RULES:
        if rx.fullmatch(item):
            return why
    return None


def item_works(item, works_ok, passed):
    for rx, steps in ITEM_STEPS:
        if rx.fullmatch(item):
            return all(s in passed for s in steps)
    return works_ok


def group_entry(name, steps, works_ok, items, passed=frozenset()):
    """items: [(item, ported_bool)]; an item that does not apply and is not
    ported is left out of the counts (listed under "na")"""
    state, na = {}, {}
    for item, ported in items:
        why = None if ported else na_reason(item)
        if why:
            na[item] = why
            continue
        works = item_works(item, works_ok, passed)
        state[item] = ("works" if works else "ported") if ported else "todo"
    count = {k: sum(1 for v in state.values() if v == k) for k in ("works", "ported", "todo")}
    return {"name": name, "steps": steps, **count, "items": dict(sorted(state.items())),
            "na": dict(sorted(na.items()))}


def summarize(groups):
    works = sum(g["works"] for g in groups)
    ported = sum(g["ported"] for g in groups)
    total = works + ported + sum(g["todo"] for g in groups)
    pct = lambda n: round(100.0 * n / total, 1) if total else 0.0
    return {"works": works, "ported": works + ported, "total": total,
            "not_applicable": sum(len(g["na"]) for g in groups),
            "works_pct": pct(works), "ported_pct": pct(works + ported), "groups": groups}


def collect_amdgpu(words, passed):
    groups = {}
    for line in INDEX.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        group, _path, fn = line.split("\t")
        groups.setdefault(group, set()).add(fn)
    out = []
    for name, fns in groups.items():
        steps = AMDGPU_STEPS.get(name, ["?"])
        out.append(group_entry(name, steps, all(s in passed for s in steps),
                               [(f, f in words) for f in fns], passed))
    return summarize(out)


def uapi_handled(src):
    """case labels -> handled? A label (or run of labels) whose first
    statement rejects (return -RD_E..., break, default) is not handled."""
    handled = {}
    pending = []
    for m in re.finditer(r"case\s+([^:]+?)\s*:|default\s*:|([^\s][^\n]*)", src):
        if m.group(1):
            pending.append(m.group(1).strip())
            continue
        if not pending:
            continue
        stmt = m.group(0).strip()
        if stmt.startswith("/*") or stmt.startswith("//"):
            continue
        reject = stmt.startswith("default") or re.match(r"(return\s+-RD_E|break\s*;)", stmt)
        for lab in pending:
            handled[lab] = handled.get(lab, False) or not reject
        pending = []
    return handled


def collect_uapi(passed):
    hdr = (ROOT / "third_party/amd/amdgpu_drm.h").read_text()
    rdh = (ROOT / "uapi/rdna3_drm.h").read_text()
    src = (ROOT / "uapi/rdna3_drm.c").read_text()
    xnu = (ROOT / "userspace/libdrm_xnu/drm_xnu.c").read_text()
    defs = re.findall(r"^\s*#define\s+([A-Z][A-Z0-9_]+)\s+(?:0x[0-9a-fA-F]+|\d+)\b", hdr, re.M)
    handled = uapi_handled(src)
    # AMDGPU_INFO_SENSOR answers come from the kext's ops.sensor
    for lab, ok in uapi_handled((ROOT / "kext/src/RDNA3Bringup.cpp").read_text()).items():
        if lab.startswith("AMDGPU_INFO_SENSOR_"):
            handled[lab] = handled.get(lab, False) or ok
    srcwords = set(re.findall(r"[A-Z][A-Z0-9_]+", src))

    def amdgpu_ioctl(n):
        return handled.get(f"RD_NR_AMDGPU({n})", False)

    kext = "".join(p.read_text(errors="ignore") for p in (ROOT / "kext/src").glob("*.cpp"))

    fwc = set(re.findall(r"AMDGPU_INFO_FW_[A-Z0-9_]+", (ROOT / "common/amd_fw.c").read_text()))

    def plain(n):
        if n.startswith("AMDGPU_INFO_FW_") and n != "AMDGPU_INFO_FW_VERSION":     # answered from dev->fw_version[], set by the kext
            if n in fwc and "amd_fw_versions(" in kext:   # from the firmware headers (amd_fw_versions)
                return True
            return re.search(r"(?:fw_version|fw_feature|ver|feat)\[\s*" + n + r"\s*\]", src + kext) is not None
        if n in handled:
            return handled[n]
        return n in srcwords

    core = [n for n in re.findall(r"#define\s+(RD_NR_[A-Z0-9_]+)", rdh)
            if n not in ("RD_NR_AMDGPU", "RD_NR_XNU_SYNCOBJ_FENCE", "RD_NR_XNU_FILE_ID")]   # libdrm_xnu helpers
    fd_in_xnu = {"RD_NR_SYNCOBJ_HANDLE_TO_FD": "XNU_NR_SYNCOBJ_HANDLE_TO_FD",
                 "RD_NR_SYNCOBJ_FD_TO_HANDLE": "XNU_NR_SYNCOBJ_FD_TO_HANDLE"}

    def core_ok(n):
        if n in fd_in_xnu:
            return fd_in_xnu[n] in xnu
        return handled.get(n, False)

    sets = {
        "amdgpu ioctls": [(n, amdgpu_ioctl(n)) for n in defs if n.startswith("DRM_AMDGPU_")],
        "DRM core ioctls": [(n.replace("RD_NR_", "DRM_IOCTL_"), core_ok(n)) for n in core],
        "INFO queries": [(n, plain(n)) for n in defs if n.startswith("AMDGPU_INFO_")
                         and (n == "AMDGPU_INFO_FW_VERSION"
                              or not re.match(r"AMDGPU_INFO_(FW_|SENSOR_|VIDEO_CAPS_|MMR_)", n))],
        "INFO firmware / sensors": [(n, plain(n)) for n in defs if n != "AMDGPU_INFO_FW_VERSION"
                                    and re.match(r"AMDGPU_INFO_(FW_|SENSOR_)", n)],
        "CS chunks": [(n, plain(n)) for n in defs if n.startswith("AMDGPU_CHUNK_ID_")],
    }
    out = []
    for name, items in sets.items():
        steps = UAPI_STEPS[name]
        out.append(group_entry(name, steps, all(s in passed for s in steps), items, passed))
    return summarize(out)


# ---------------------------------------------------------------- render

def squarify(values, x, y, w, h):
    """treemap rectangles, same order as values (sorted descending)"""
    rects, rest = [], list(values)
    total = float(sum(rest))
    scale = w * h / total if total else 0
    areas = [v * scale for v in rest]

    def worst(row, side):
        s = sum(row)
        return max(max(side * side * r / (s * s), s * s / (side * side * r)) for r in row)

    row = []
    while areas:
        side = min(w, h)
        if not row or worst(row + [areas[0]], side) <= worst(row, side):
            row.append(areas.pop(0))
            continue
        x, y, w, h = lay(row, x, y, w, h, rects)
        row = []
    if row:
        lay(row, x, y, w, h, rects)
    return rects


def lay(row, x, y, w, h, rects):
    s = sum(row)
    if w >= h:
        thick = s / h
        off = 0.0
        for a in row:
            rects.append((x, y + off, thick, a / thick))
            off += a / thick
        return x + thick, y, w - thick, h
    thick = s / w
    off = 0.0
    for a in row:
        rects.append((x + off, y, a / thick, thick))
        off += a / thick
    return x, y + thick, w, h - thick


def grid(n, x, y, w, h):
    if n <= 0:
        return []
    rows = max(1, min(n, round((n * h / w) ** 0.5)))
    cells = []
    for r in range(rows):
        k = n // rows + (1 if r < n % rows else 0)
        cw = w / k
        cells += [(x + c * cw, y + r * h / rows, cw, h / rows) for c in range(k)]
    return cells


def rect(x, y, w, h, fill, stroke=BG, sw=0.5):
    return (f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="{sw}"/>')


def label(x, y, s, size, fill=TEXT):
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-family="Verdana,DejaVu Sans,sans-serif" '
            f'font-size="{size}" fill="{fill}" stroke="{BG}" stroke-width="3" '
            f'paint-order="stroke">{escape(s)}</text>')


def panel(title, data, left):
    out = [label(left + 4, 20, title, 15),
           label(left + 4, 42, f'works {data["works_pct"]}%   ported {data["ported_pct"]}%   '
                 f'({data["works"]} / {data["ported"]} / {data["total"]})', 12)]
    groups = sorted((g for g in data["groups"] if g["works"] + g["ported"] + g["todo"]),
                    key=lambda g: -(g["works"] + g["ported"] + g["todo"]))
    sizes = [g["works"] + g["ported"] + g["todo"] for g in groups]
    for g, (x, y, w, h) in zip(groups, squarify(sizes, left, HEAD, PANEL_W, MAP_H)):
        n = g["works"] + g["ported"] + g["todo"]
        pct_w = 100 * g["works"] / n
        pct_p = 100 * (g["works"] + g["ported"]) / n
        out.append(f'<g><title>{escape(g["name"])}: works {pct_w:.0f}%, ported {pct_p:.0f}% '
                   f'({g["works"]}+{g["ported"]} of {n}); verified by step(s) '
                   f'{escape(", ".join(g["steps"]))}</title>')
        for i, c in enumerate(grid(n, x + 1, y + 1, w - 2, h - 2)):
            out.append(rect(*c, GREEN if i < g["works"] else YELLOW if i < g["works"] + g["ported"] else GRAY))
        out.append(rect(x, y, w, h, "none", BG, 2))
        name = g["name"] if len(g["name"]) * 7 < w - 8 else g["name"][:max(0, int((w - 14) / 7))] + "…"
        if h > 34 and w > 40:
            out.append(label(x + 5, y + 15, name, 11))
            out.append(label(x + 5, y + 29, f"{pct_w:.0f}% / {pct_p:.0f}%", 10))
        out.append("</g>")
    return out


def legend(y):
    x, out = 4, []
    for color, text in ((GREEN, "works (verified on the card)"),
                        (YELLOW, "ported, not verified on the card yet"), (GRAY, "not ported")):
        out.append(rect(x, y, 12, 12, color, BG, 0))
        out.append(label(x + 17, y + 11, text, 11))
        x += 30 + 7 * len(text)
    return out


def render(amdgpu, uapi):
    width = 2 * PANEL_W + GAP
    height = HEAD + MAP_H + 24
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
           f'width="{width}" height="{height}">', rect(0, 0, width, height, BG, BG, 0)]
    out += panel("amdgpu kernel functions*", amdgpu, 0)
    out += panel("amdgpu UAPI (ioctls, INFO, CS chunks)", uapi, PANEL_W + GAP)
    out += legend(HEAD + MAP_H + 6)
    out.append("</svg>")
    return "\n".join(out) + "\n"


def badge(name, data):
    """three-part badge: name | works X% | ported Y%"""
    w_txt, p_txt = f'works {data["works_pct"]}%', f'ported {data["ported_pct"]}%'
    a, b, c = (10 + 7 * len(s) for s in (name, w_txt, p_txt))
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{a + b + c}" height="20" role="img" '
            f'aria-label="{name}: {w_txt}, {p_txt}">'
            f'<rect width="{a}" height="20" fill="#555"/>'
            f'<rect x="{a}" width="{b}" height="20" fill="{GREEN}"/>'
            f'<rect x="{a + b}" width="{c}" height="20" fill="{YELLOW}"/>'
            f'<g fill="#fff" font-family="Verdana,DejaVu Sans,sans-serif" font-size="11" text-anchor="middle">'
            f'<text x="{a / 2}" y="14">{name}</text><text x="{a + b / 2}" y="14">{w_txt}</text>'
            f'<text x="{a + b + c / 2}" y="14">{p_txt}</text></g></svg>\n')


def markdown(amdgpu, uapi, passed):
    def table(title, col, data):
        rows = [f"## {title}", "",
                f'{data["not_applicable"]} items that do not apply (not on the RX 7800 XT, or of no use on macOS) '
                "are left out of the totals; each group lists them with the reason "
                "(`tools/progress/not-applicable.txt`).", "",
                f"| {col} | Works | Ported | Total | Works % | Ported % | Verified by step |",
                "|---|---:|---:|---:|---:|---:|---|"]
        for g in sorted(data["groups"], key=lambda g: g["name"].lower()):
            n = g["works"] + g["ported"] + g["todo"]
            rows.append(f'| {g["name"]} | {g["works"]} | {g["works"] + g["ported"]} | {n} | '
                        f'{100 * g["works"] / n:.0f}% | {100 * (g["works"] + g["ported"]) / n:.0f}% | '
                        f'{", ".join(g["steps"])} |')
        rows.append(f'| **Total** | {data["works"]} | {data["ported"]} | {data["total"]} | '
                    f'{data["works_pct"]}% | {data["ported_pct"]}% | |')
        rows.append("")
        for g in sorted(data["groups"], key=lambda g: g["name"].lower()):
            todo = [k for k, v in g["items"].items() if v == "todo"]
            done = [k for k, v in g["items"].items() if v != "todo"]
            na_txt = f', {len(g["na"])} do not apply' if g["na"] else ""
            rows.append(f'<details><summary>{g["name"]}: {len(done)} ported, {len(todo)} to do{na_txt}</summary>\n')
            if done:
                rows.append("Ported: " + ", ".join(f"`{k}`" for k in done) + "\n")
            if todo:
                rows.append("To do: " + ", ".join(f"`{k}`" for k in todo) + "\n")
            if g["na"]:
                by = {}
                for k, why in g["na"].items():
                    by.setdefault(why, []).append(k)
                rows.append("Not counted (does not apply):\n")
                for why, ks in by.items():
                    rows.append(f"- {why}: " + ", ".join(f"`{k}`" for k in ks))
                rows.append("")
            rows.append("</details>\n")
        return rows

    lines = ["# Porting status", "",
             "Generated by `tools/progress.py` (do not edit). Steps passed on the card: "
             + (", ".join(sorted(passed)) if passed else "none yet") + " (`docs/status/hw-verified.txt`).", "",
             "![progress map](progress.svg)", ""]
    lines += table("amdgpu kernel functions", "Block", amdgpu)
    lines += table("amdgpu UAPI", "Group", uapi)
    return "\n".join(lines) + "\n"


def generate(out):
    passed = verified_steps()
    words = driver_words()
    amdgpu, uapi = collect_amdgpu(words, passed), collect_uapi(passed)
    out.mkdir(parents=True, exist_ok=True)
    (out / "progress.json").write_text(json.dumps({"amdgpu": amdgpu, "uapi": uapi,
                                                   "verified_steps": sorted(passed)}, indent=1) + "\n")
    (out / "progress.svg").write_text(render(amdgpu, uapi))
    (out / "badge-amdgpu.svg").write_text(badge("amdgpu port*", amdgpu))
    (out / "badge-uapi.svg").write_text(badge("amdgpu UAPI", uapi))
    (out / "STATUS.md").write_text(markdown(amdgpu, uapi, passed))
    return amdgpu, uapi


def main():
    if "--check" in sys.argv:
        with tempfile.TemporaryDirectory() as t:
            generate(Path(t))
            stale = [f.name for f in Path(t).iterdir()
                     if not (OUT / f.name).exists() or (OUT / f.name).read_text() != f.read_text()]
        if stale:
            print("docs/status is stale (run python3 tools/progress.py):", ", ".join(sorted(stale)))
            sys.exit(1)
        print("docs/status is up to date")
        return
    amdgpu, uapi = generate(OUT)
    for n, d in (("amdgpu", amdgpu), ("uapi", uapi)):
        print(f'{n}: works {d["works"]} ({d["works_pct"]}%), ported {d["ported"]} '
              f'({d["ported_pct"]}%) of {d["total"]}')


if __name__ == "__main__":
    main()
