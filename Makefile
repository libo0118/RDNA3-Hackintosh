# Host-side build: portable code shared with the kext, its tests and tools.
# The kext itself is built on macOS: see kext/Makefile.

CC      ?= cc
CFLAGS  ?= -O1 -g
WARN    := -std=c99 -Wall -Wextra -Werror -Wshadow -Wcast-align -Wconversion -Wno-sign-conversion
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
INC     := -Icommon -Ihw -Iuapi -Ithird_party/amd
BUILD   := build/host

COMMON  := common/amd_discovery.c common/amd_fw.c common/range_alloc.c
HW      := hw/rdna3_hw.c hw/psp13.c hw/smu13.c hw/gmc11.c hw/mmhub302.c hw/ih60.c hw/sdma60.c hw/gpuvm.c hw/gfx11.c hw/dcn32.c hw/doorbell.c hw/vbios.c hw/dcaux.c hw/edid.c hw/atom.c hw/dmub32.c hw/dcn32_mode.c hw/dcn32_clone.c hw/dcn32_modeset.c hw/dcn32_clk.c hw/gfx11_info.c hw/xfer.c hw/soc21_cg.c
HW      += hw/dchpd.c
HW      += hw/dcirq.c
HW      += hw/dcn32_gamma.c
HW      += hw/dcn32_state.c
HW      += hw/dcn32_dp.c
HW      += hw/vcn40.c
HW      += hw/dcn32_dsc.c hw/dp_mst.c hw/dcn32_dp_mst.c
HW      += hw/dcn32_otg.c hw/dcn32_color.c hw/dcn32_hdmi.c hw/dcn32_hwseq.c
HW      += hw/dcn32_hpo.c hw/dcn32_odm.c
HW      += hw/dcn32_audio.c hw/dcn32_opp.c
HW      += hw/dcn32_pstate.c hw/dcn32_subvp.c hw/dcn32_freesync.c
HW      += hw/dcn32_txstate.c
HW      += hw/smu13_0_7.c
UAPI    := uapi/rdna3_drm.c
COMMON_H := $(wildcard common/*.h) $(wildcard hw/*.h) $(wildcard uapi/*.h)
COMMON_H += $(wildcard hw/gen/*smu*.h)

.PHONY: all test test-run tools kext clean

all: test tools

$(BUILD):
	mkdir -p $@

$(BUILD)/test_discovery: tests/test_discovery.c $(COMMON) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Wno-conversion $(SAN) $(INC) -o $@ tests/test_discovery.c $(COMMON)

$(BUILD)/amd_discovery_dump: tools/amd_discovery_dump.c $(COMMON) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) -o $@ tools/amd_discovery_dump.c $(COMMON)

$(BUILD)/test_fw: tests/test_fw.c $(COMMON) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Wno-conversion $(SAN) $(INC) -o $@ tests/test_fw.c $(COMMON)

$(BUILD)/amd_fw_dump: tools/amd_fw_dump.c $(COMMON) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) -o $@ tools/amd_fw_dump.c $(COMMON)

$(BUILD)/sim_navi32: tests/sim_navi32.c tests/sim_display_dc.inc tests/sim_display_hpo.inc tests/sim_display_audio.inc tests/sim_display_pstate.inc tests/sim_display_freesync.inc tests/sim_dc_irq.inc $(COMMON) $(HW) $(UAPI) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Wno-conversion -Wno-cast-align $(SAN) $(INC) -o $@ tests/sim_navi32.c $(COMMON) $(HW) $(UAPI)

$(BUILD)/test_smu13: tests/test_smu13.c tests/sim_smu13_0_7.inc $(COMMON) $(HW) $(UAPI) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Wno-conversion -Wno-cast-align $(SAN) $(INC) -o $@ tests/test_smu13.c $(COMMON) $(HW) $(UAPI)

$(BUILD)/test_uapi: tests/test_uapi.c $(COMMON) $(UAPI) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Wno-conversion -Wno-cast-align $(SAN) $(INC) -o $@ tests/test_uapi.c $(COMMON) $(UAPI)

$(BUILD)/test_agdc_k5: tests/test_agdc_k5.c agdc/rdna3_agdc_k5.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -o $@ tests/test_agdc_k5.c
# DRIVER-PLAN N45: the AGDC answers after Navi48-MacOS, per switch value
$(BUILD)/test_agdc_n48: tests/test_agdc_n48.c agdc/rdna3_agdc_n48.h agdc/rdna3_agdc_k5.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -o $@ tests/test_agdc_n48.c

# common/rdna3_bootargs.h with a stubbed PE_parse_boot_argn (tests/stub)
$(BUILD)/test_bootargs: tests/test_bootargs.c common/rdna3_bootargs.h tests/stub/pexpert/pexpert.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Itests/stub -Icommon -o $@ tests/test_bootargs.c
# common/rdna3_a3_display.h: the surfaces A3 keeps out (card run 58)
$(BUILD)/test_a3_display: tests/test_a3_display.c common/rdna3_a3_display.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_a3_display.c
# common/rdna3_swap_front.h: the swap's buffer and copy forward's source (card run 58b)
$(BUILD)/test_attr_ring: tests/test_attr_ring.c common/rdna3_attr_ring.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_attr_ring.c
# common/rdna3_bar_window.h: the kernel window of BAR0 (N62, card run 2026-10-28c)
$(BUILD)/test_bar_window: tests/test_bar_window.c common/rdna3_bar_window.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_bar_window.c

$(BUILD)/test_swap_front: tests/test_swap_front.c common/rdna3_swap_front.h common/rdna3_a3_display.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_swap_front.c

$(BUILD)/test_handoff: tests/test_handoff.c metal/plugin/nvmtl_handoff.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_handoff.c

$(BUILD)/test_present_cost: tests/test_present_cost.c metal/plugin/nvmtl_present_cost.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_present_cost.c
$(BUILD)/test_drawable_life: tests/test_drawable_life.c metal/plugin/nvmtl_drawable_life.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_drawable_life.c
$(BUILD)/test_ws_frame: tests/test_ws_frame.c metal/plugin/nvmtl_ws_frame.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_ws_frame.c
$(BUILD)/test_strip: tests/test_strip.c common/rdna3_strip.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_strip.c
$(BUILD)/test_iop_stale: tests/test_iop_stale.c common/rdna3_iop_stale.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_iop_stale.c

$(BUILD)/test_iop_copy: tests/test_iop_copy.c common/rdna3_iop_copy.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_iop_copy.c

$(BUILD)/test_iop_latch: tests/test_iop_latch.c common/rdna3_iop_latch.h common/rdna3_flip_queue.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_iop_latch.c
$(BUILD)/test_flip_latch: tests/test_flip_latch.c common/rdna3_flip_latch.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_flip_latch.c
# DRIVER-PLAN N42: IOGraphics' VBL from the vblank interrupt, the poll as fallback
$(BUILD)/test_tcall: tests/test_tcall.c common/rdna3_tcall.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_tcall.c
# DRIVER-PLAN N58: the framebuffers' controller gates before the display machine (card run 2026-10-26j)
$(BUILD)/test_fbgate: tests/test_fbgate.c common/rdna3_fbgate.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_fbgate.c

$(BUILD)/test_vbl_irq: tests/test_vbl_irq.c common/rdna3_vbl_irq.h common/rdna3_fb_policy.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_vbl_irq.c
# DRIVER-PLAN N38: a head's presents without blocking WindowServer (the flip queue)
$(BUILD)/test_gpu_busy: tests/test_gpu_busy.c common/rdna3_gpu_busy.h common/rdna3_gpu_util.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_gpu_busy.c
$(BUILD)/test_coh: tests/test_coh.c common/rdna3_coh.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_coh.c

$(BUILD)/test_flip_queue: tests/test_flip_queue.c common/rdna3_flip_queue.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_flip_queue.c

$(BUILD)/test_fb_policy: tests/test_fb_policy.c common/rdna3_fb_policy.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_fb_policy.c
# card run 2026-10-17b: the swap as a copy (mode 1) and the per-frame trace
$(BUILD)/test_swap_copy: tests/test_swap_copy.c common/rdna3_swap_copy.h common/rdna3_swap_front.h common/rdna3_flip_latch.h common/rdna3_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_swap_copy.c
$(BUILD)/test_write_gen: tests/test_write_gen.c common/rdna3_write_gen.h common/rdna3_swap_copy.h common/rdna3_swap_front.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_write_gen.c
# DRIVER-PLAN N41: a surface's VRAM kept across the family's dealloc/alloc, and WindowServer's pending GPU writes
$(BUILD)/test_vram_park: tests/test_vram_park.c common/rdna3_vram_park.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_vram_park.c
$(BUILD)/test_surf_pending: tests/test_surf_pending.c common/rdna3_surf_pending.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_surf_pending.c
$(BUILD)/test_trace: tests/test_trace.c common/rdna3_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_trace.c
# common/rdna3_gamma_judge.h: rdna3_modetest's gamma verdict (card run 58)
$(BUILD)/test_gamma_judge: tests/test_gamma_judge.c common/rdna3_gamma_judge.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_gamma_judge.c
# the Metal plugin's allow-list policy is plain C, tested on the host
$(BUILD)/test_mtl_allow: tests/test_mtl_allow.c metal/plugin/rdna3mtl_allow_policy.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_mtl_allow.c

# the linear buffer texture's pitch rule (RADV: a multiple of 256 B), card runs 2026-10-17d / 18b
$(BUILD)/test_linear_pitch: tests/test_linear_pitch.c metal/plugin/nvmtl_linear_pitch.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_linear_pitch.c
$(BUILD)/test_gpu_time_queue: tests/test_gpu_time_queue.c metal/plugin/nvmtl_gpu_time_queue.h metal/plugin/nvmtl_log_failure.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_gpu_time_queue.c

$(BUILD)/test_drawable_defer: tests/test_drawable_defer.c metal/plugin/nvmtl_drawable_defer.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_drawable_defer.c
$(BUILD)/test_gsub_empty: tests/test_gsub_empty.c metal/plugin/nvmtl_gsub_empty.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_gsub_empty.c
$(BUILD)/test_surface_share: tests/test_surface_share.c metal/plugin/nvmtl_surface_share.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_surface_share.c
$(BUILD)/test_ws_share: tests/test_ws_share.c metal/plugin/nvmtl_ws_share.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_ws_share.c

$(BUILD)/test_ab_switch: tests/test_ab_switch.c metal/plugin/nvmtl_ab_switch.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_ab_switch.c

$(BUILD)/test_surf_census: tests/test_surf_census.c metal/plugin/nvmtl_surf_census.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_surf_census.c

$(BUILD)/test_surfin_cache: tests/test_surfin_cache.c metal/plugin/nvmtl_surfin_cache.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_surfin_cache.c

$(BUILD)/test_ci_refl: tests/test_ci_refl.c metal/plugin/nvmtl_ci_refl.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_ci_refl.c

$(BUILD)/test_spirv_fbfetch: tests/test_spirv_fbfetch.c metal/plugin/nvmtl_spirv_fbfetch.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_spirv_fbfetch.c

$(BUILD)/test_fbfetch_plan: tests/test_fbfetch_plan.c metal/plugin/nvmtl_fbfetch_plan.h metal/plugin/nvmtl_log_cap.h metal/plugin/nvmtl_log_failure.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_fbfetch_plan.c

$(BUILD)/test_msupload: tests/test_msupload.c metal/plugin/nvmtl_msupload.h metal/plugin/nvmtl_msbuf.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_msupload.c

$(BUILD)/test_msdl: tests/test_msdl.c metal/plugin/nvmtl_msdl.h metal/plugin/nvmtl_msbuf.h metal/plugin/nvmtl_msstat.h metal/plugin/nvmtl_msupload.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_msdl.c

$(BUILD)/test_capture: tests/test_capture.c metal/plugin/nvmtl_capture.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_capture.c
$(BUILD)/test_copy_pack: tests/test_copy_pack.c metal/plugin/nvmtl_copy_pack.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_copy_pack.c

$(BUILD)/test_range_alloc: tests/test_range_alloc.c $(COMMON) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Wno-conversion $(SAN) $(INC) -o $@ tests/test_range_alloc.c $(COMMON)
# card run 2026-10-17c: where VRAM goes (the CPU-visible heap's reserve, no-CPU BOs stay out)
# DRIVER-PLAN N47 (b): a CPU mapping holds its BO (common/rdna3_bo_map.h with the real UAPI)
$(BUILD)/test_bo_map: tests/test_bo_map.c common/rdna3_bo_map.h $(COMMON) $(UAPI) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Wno-conversion -Wno-cast-align $(SAN) $(INC) -o $@ tests/test_bo_map.c $(COMMON) $(UAPI)

# the descriptor's release protocol under threads (ECC review round 1 of N47 (b))
$(BUILD)/test_bo_map_race: tests/test_bo_map_race.c common/rdna3_bo_map.h $(COMMON) $(UAPI) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -std=c11 -Wno-conversion -Wno-cast-align $(SAN) -pthread $(INC) -o $@ tests/test_bo_map_race.c $(COMMON) $(UAPI)

$(BUILD)/test_vram_place: tests/test_vram_place.c common/rdna3_vram_place.h accel/rdna3k_abi.h $(COMMON) $(UAPI) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Wno-conversion -Wno-cast-align $(SAN) $(INC) -o $@ tests/test_vram_place.c $(COMMON) $(UAPI)

# Real libdrm_amdgpu over drm_xnu (loopback backend into uapi/) on the host
LIBDRM      := third_party/libdrm
LIBDRM_SRC  := $(filter-out $(LIBDRM)/amdgpu/amdgpu_userq.c,$(wildcard $(LIBDRM)/amdgpu/*.c)) $(LIBDRM)/amdgpu/amdgpu_userq.c
XNU         := userspace/libdrm_xnu
LIBDRM_INC  := -I$(XNU)/override -I$(XNU) -I$(LIBDRM) -I$(LIBDRM)/amdgpu -I$(LIBDRM)/include/drm
LIBDRM_DEFS := -DHAVE_LIBDRM_ATOMIC_PRIMITIVES=1 -DAMDGPU_ASIC_ID_TABLE='"$(CURDIR)/$(LIBDRM)/data/amdgpu.ids"' -D_GNU_SOURCE

$(BUILD)/libdrm-host/%.o: $(LIBDRM)/amdgpu/%.c | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu11 $(SAN) -fno-sanitize=undefined $(LIBDRM_INC) $(LIBDRM_DEFS) -w -c $< -o $@

$(BUILD)/libdrm-host/xnu_%.o: $(XNU)/%.c $(wildcard $(XNU)/*.h) | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror $(SAN) $(LIBDRM_INC) -D_GNU_SOURCE -c $< -o $@

# the loopback backend talks to uapi/, which uses the Mesa/kernel UAPI headers
$(BUILD)/libdrm-host/loopback.o: $(XNU)/backend_loopback.c $(COMMON_H) | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror $(SAN) -I$(XNU) $(INC) -D_GNU_SOURCE -c $< -o $@

LIBDRM_HOST_OBJS := $(patsubst $(LIBDRM)/amdgpu/%.c,$(BUILD)/libdrm-host/%.o,$(LIBDRM_SRC)) \
                    $(BUILD)/libdrm-host/xnu_drm_xnu.o $(BUILD)/libdrm-host/xnu_xf86drm_xnu.o \
                    $(BUILD)/libdrm-host/loopback.o

# uapi/ and common/ are built against the kernel/Mesa UAPI headers, not libdrm's copies
$(BUILD)/libdrm-host/k_%.o: uapi/%.c $(COMMON_H) | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu11 $(SAN) $(INC) -c $< -o $@
$(BUILD)/libdrm-host/c_%.o: common/%.c $(COMMON_H) | $(BUILD)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu11 $(SAN) $(INC) -c $< -o $@
KOBJS := $(patsubst uapi/%.c,$(BUILD)/libdrm-host/k_%.o,$(UAPI)) \
         $(patsubst common/%.c,$(BUILD)/libdrm-host/c_%.o,$(COMMON))

$(BUILD)/test_libdrm_amdgpu: tests/test_libdrm_amdgpu.c $(LIBDRM_HOST_OBJS) $(KOBJS) | $(BUILD)
	$(CC) $(CFLAGS) -std=gnu11 $(SAN) $(LIBDRM_INC) -D_GNU_SOURCE -o $@ tests/test_libdrm_amdgpu.c \
	  $(LIBDRM_HOST_OBJS) $(KOBJS) -lpthread

# rdna3_drminfo on the loopback: --scanout writes the loopback's test picture
$(BUILD)/rdna3_drminfo_host: userspace/rdna3_drminfo.c common/rdna3_strip.h $(LIBDRM_HOST_OBJS) $(KOBJS) | $(BUILD)
	$(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror $(SAN) $(LIBDRM_INC) $(INC) -D_GNU_SOURCE -o $@ \
	  userspace/rdna3_drminfo.c $(LIBDRM_HOST_OBJS) $(KOBJS) -lpthread

# ---- make test: one target per test, run in parallel -----------------------------------------------------------
# `make test` runs every test as its own target `t.<name>` (a test starts as soon as its binary is built), with
# one job per hardware thread when no -j is given: -jN / JOBS=N choose another number, `make -j1 test` or JOBS=1
# is serial. The output of each test goes to $(TESTLOG)/<name>.log (tests/run_one.sh), failed logs are printed
# at the end with one summary line (tests/test_summary.sh); the exit status is 1 if any test failed.
# RDNA3_FW_DIR=<linux-firmware/amdgpu> additionally checks real Navi 32 files.
#
# Every test works in its own mktemp directory (or in $(BUILD) under a name only it uses), so any order and any
# number of jobs is fine. Two tests are one target because their steps depend on each other: t.test_capture (the
# writer, then the decoder reads its file) and t.test_scanout (tests/test_scanout.sh: the loopback's picture,
# then what reads it).
TESTLOG := build/test-logs
# `make test` has to hand -jN to a recursive make: a -j set inside the Makefile is not reliable on make 3.81
# (the macOS runner's). Detect the user's own -j in MAKEFLAGS (any spelling contains "-j"); nproc is GNU, hw.ncpu BSD.
NPROC    = $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
TESTJOBS = $(if $(findstring -j,$(MAKEFLAGS)),,-j$(if $(JOBS),$(JOBS),$(NPROC)))

# the tests without arguments: shell and python tests (the slow ones first, they need no build and run while the
# binaries compile; make starts the targets in this order), then the binaries
C_TESTS := test_discovery test_smu13 test_uapi test_range_alloc test_vram_place test_bo_map test_bo_map_race \
    test_linear_pitch test_gpu_time_queue test_ab_switch test_drawable_defer test_gsub_empty test_surface_share test_ws_share test_surf_census test_surfin_cache test_ci_refl \
    test_copy_pack test_msupload test_msdl test_spirv_fbfetch test_fbfetch_plan test_agdc_k5 test_agdc_n48 \
    test_bootargs test_a3_display test_gamma_judge test_swap_front test_attr_ring test_bar_window \
    test_flip_latch test_iop_stale test_iop_copy test_iop_latch test_handoff test_present_cost test_ws_frame test_drawable_life test_vbl_irq \
    test_tcall test_fbgate test_flip_queue test_gpu_busy test_coh test_fb_policy test_swap_copy test_write_gen \
    test_vram_park test_surf_pending test_trace test_libdrm_amdgpu test_strip
PY_TESTS := test_docs_html test_glyph_verdict test_lookimg test_triage_parallel test_accel_hash_parity \
    test_fb_agdc_hash_parity test_trace_parity test_triage_park test_triage_n43 test_triage_gpuq \
    test_triage_n48 test_triage_bootwatch test_triage_glyphs test_triage_coherence test_triage_attr_ring \
    test_triage_bar test_triage_present_cost test_triage_write_gen test_fb_class test_triage_iop_log \
    test_triage_agdc_n48 test_triage_pairing test_triage_nocard test_kext_kpi_versions test_kmutil_kpi_guard \
    test_triage_tcall test_triage_bomem test_accel_slots test_accel_locks test_triage_apps test_iop_join \
    test_triage_uclk_pin test_fw_dump test_pm_property
SH_TESTS := test_bootwatch test_membw_load test_triage_a3 \
    test_baremetal test_triage_trace test_oc_install test_make_pkg test_coredisplay_prefs test_prune_traces \
    test_triage_tx test_triage_stale_plugin test_triage_latch test_result_checklist test_triage_vram \
    test_plugin_trace test_triage_bisect test_play_step test_approve_gate test_forgejo_update \
    test_triage_stale_kexts test_vm_present_ab test_vm_amd_cpu test_triage_present test_triage_fb_n40 \
    test_probe_coredisplay test_fetch_fw
define TEST_RULE
.PHONY: t.$(1)
t.$(1): $(2)
	@sh tests/run_one.sh $(TESTLOG) $(1) $(3)
TEST_TARGETS += t.$(1)
endef
TEST_TARGETS :=
# The three step tests are the slowest ones (minutes on the macOS runner) and run in parts, one target each,
# t.<script>.<part> (tests/split_parts.sh: no argument = every part in turn, a part name = only that part, --parts
# lists them; the lists are the scripts' PARTS). The dash parts say "skip" when dash is not installed.
BISECT_PARTS := $(shell bash tests/test_bisect_step.sh --parts)
LADDER_PARTS := $(shell bash tests/test_ladder_step.sh --parts)
APPS_PARTS   := $(shell bash tests/test_apps_step.sh --parts)
$(foreach p,$(BISECT_PARTS),$(eval $(call TEST_RULE,test_bisect_step.$(p),,tests/test_bisect_step.sh $(p))))
$(foreach p,$(LADDER_PARTS),$(eval $(call TEST_RULE,test_ladder_step.$(p),,tests/test_ladder_step.sh $(p))))
$(foreach p,$(APPS_PARTS),$(eval $(call TEST_RULE,test_apps_step.$(p),,tests/test_apps_step.sh $(p))))
$(foreach t,$(SH_TESTS),$(eval $(call TEST_RULE,$(t),,tests/$(t).sh)))
$(foreach t,$(PY_TESTS),$(eval $(call TEST_RULE,$(t),,python3 -I -B tests/$(t).py)))
t.test_fw_dump: $(BUILD)/amd_fw_dump
$(foreach t,$(C_TESTS),$(eval $(call TEST_RULE,$(t),$(BUILD)/$(t),$(BUILD)/$(t))))
$(eval $(call TEST_RULE,test_fw,$(BUILD)/test_fw,$(BUILD)/test_fw $(RDNA3_FW_DIR)))
$(eval $(call TEST_RULE,sim_navi32,$(BUILD)/sim_navi32,$(BUILD)/sim_navi32 $(RDNA3_FW_DIR)))
$(eval $(call TEST_RULE,test_mtl_allow,$(BUILD)/test_mtl_allow,$(BUILD)/test_mtl_allow metal/rdna3mtl-allow.txt))
$(eval $(call TEST_RULE,test_ocpatchsim,,python3 -I tests/test_ocpatchsim.py))
$(eval $(call TEST_RULE,test_capture,$(BUILD)/test_capture,sh tests/test_capture_decode.sh $(BUILD)))
$(eval $(call TEST_RULE,test_scanout,$(BUILD)/rdna3_drminfo_host,sh tests/test_scanout.sh $(BUILD)))
# two jobs in one process each get their own VA (card run 2026-10-12)
$(eval $(call TEST_RULE,test_drminfo_va,$(BUILD)/rdna3_drminfo_host,sh tests/test_drminfo_va.sh $(BUILD)))

# -k: a binary that does not compile must not stop the others, and the summary (the failed logs, the verdict) runs
# whatever happened; a test whose binary did not build has no result there and is counted as failed
test:
	@rm -rf $(TESTLOG)
	@$(MAKE) -k $(TESTJOBS) test-run; sh tests/test_summary.sh $(TESTLOG) $(patsubst t.%,%,$(TEST_TARGETS))

test-run: $(TEST_TARGETS)

tools: $(BUILD)/amd_discovery_dump $(BUILD)/amd_fw_dump

kext:
	$(MAKE) -C kext

clean:
	rm -rf build
	$(MAKE) -C kext clean 2>/dev/null || true
