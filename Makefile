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
UAPI    := uapi/rdna3_drm.c
COMMON_H := $(wildcard common/*.h) $(wildcard hw/*.h) $(wildcard uapi/*.h)

.PHONY: all test tools kext clean

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

$(BUILD)/sim_navi32: tests/sim_navi32.c tests/sim_display_dc.inc tests/sim_display_hpo.inc tests/sim_display_audio.inc tests/sim_display_pstate.inc tests/sim_display_freesync.inc $(COMMON) $(HW) $(UAPI) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Wno-conversion -Wno-cast-align $(SAN) $(INC) -o $@ tests/sim_navi32.c $(COMMON) $(HW) $(UAPI)

$(BUILD)/test_uapi: tests/test_uapi.c $(COMMON) $(UAPI) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Wno-conversion -Wno-cast-align $(SAN) $(INC) -o $@ tests/test_uapi.c $(COMMON) $(UAPI)

$(BUILD)/test_agdc_k5: tests/test_agdc_k5.c agdc/rdna3_agdc_k5.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -o $@ tests/test_agdc_k5.c

# common/rdna3_bootargs.h with a stubbed PE_parse_boot_argn (tests/stub)
$(BUILD)/test_bootargs: tests/test_bootargs.c common/rdna3_bootargs.h tests/stub/pexpert/pexpert.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Itests/stub -Icommon -o $@ tests/test_bootargs.c
# common/rdna3_a3_display.h: the surfaces A3 keeps out (card run 58)
$(BUILD)/test_a3_display: tests/test_a3_display.c common/rdna3_a3_display.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_a3_display.c
# common/rdna3_swap_front.h: the swap's buffer and copy forward's source (card run 58b)
$(BUILD)/test_swap_front: tests/test_swap_front.c common/rdna3_swap_front.h common/rdna3_a3_display.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_swap_front.c

$(BUILD)/test_flip_latch: tests/test_flip_latch.c common/rdna3_flip_latch.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_flip_latch.c
# card run 2026-10-17b: the swap as a copy (mode 1) and the per-frame trace
$(BUILD)/test_swap_copy: tests/test_swap_copy.c common/rdna3_swap_copy.h common/rdna3_swap_front.h common/rdna3_flip_latch.h common/rdna3_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_swap_copy.c
$(BUILD)/test_trace: tests/test_trace.c common/rdna3_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_trace.c
# common/rdna3_gamma_judge.h: rdna3_modetest's gamma verdict (card run 58)
$(BUILD)/test_gamma_judge: tests/test_gamma_judge.c common/rdna3_gamma_judge.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Icommon -o $@ tests/test_gamma_judge.c
# the Metal plugin's allow-list policy is plain C, tested on the host
$(BUILD)/test_mtl_allow: tests/test_mtl_allow.c metal/plugin/rdna3mtl_allow_policy.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) -Imetal/plugin -o $@ tests/test_mtl_allow.c

$(BUILD)/test_range_alloc: tests/test_range_alloc.c $(COMMON) $(COMMON_H) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Wno-conversion $(SAN) $(INC) -o $@ tests/test_range_alloc.c $(COMMON)
# card run 2026-10-17c: where VRAM goes (the CPU-visible heap's reserve, no-CPU BOs stay out)
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
$(BUILD)/rdna3_drminfo_host: userspace/rdna3_drminfo.c $(LIBDRM_HOST_OBJS) $(KOBJS) | $(BUILD)
	$(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror $(SAN) $(LIBDRM_INC) $(INC) -D_GNU_SOURCE -o $@ \
	  userspace/rdna3_drminfo.c $(LIBDRM_HOST_OBJS) $(KOBJS) -lpthread

# RDNA3_FW_DIR=<linux-firmware/amdgpu> additionally checks real Navi 32 files
test: $(BUILD)/test_agdc_k5 $(BUILD)/test_bootargs $(BUILD)/test_a3_display $(BUILD)/test_gamma_judge $(BUILD)/test_swap_front $(BUILD)/test_flip_latch $(BUILD)/test_swap_copy $(BUILD)/test_trace $(BUILD)/test_discovery $(BUILD)/test_fw $(BUILD)/sim_navi32 $(BUILD)/test_uapi $(BUILD)/test_range_alloc $(BUILD)/test_vram_place $(BUILD)/test_mtl_allow $(BUILD)/test_libdrm_amdgpu $(BUILD)/rdna3_drminfo_host
	$(BUILD)/test_discovery
	$(BUILD)/test_fw $(RDNA3_FW_DIR)
	$(BUILD)/sim_navi32 $(RDNA3_FW_DIR)
	$(BUILD)/test_uapi
	$(BUILD)/test_range_alloc
	$(BUILD)/test_vram_place
	$(BUILD)/test_mtl_allow metal/rdna3mtl-allow.txt
	$(BUILD)/test_agdc_k5
	$(BUILD)/test_bootargs
	$(BUILD)/test_a3_display
	$(BUILD)/test_gamma_judge
	$(BUILD)/test_swap_front
	$(BUILD)/test_flip_latch
	$(BUILD)/test_swap_copy
	$(BUILD)/test_trace
	$(BUILD)/test_libdrm_amdgpu
	rm -f $(BUILD)/scanout-pipe*.ppm
	$(BUILD)/rdna3_drminfo_host --scanout $(BUILD) | grep -q '1 pipe(s) lit'
	$(BUILD)/rdna3_drminfo_host --scanout $(BUILD) | grep -q '^  TX problems: none$$'
	$(BUILD)/rdna3_drminfo_host --scanout-hires $(BUILD) | grep -q '1 pipe(s) lit'
	python3 tools/triage_reports.py --picture $(BUILD)/scanout-pipe0.ppm | grep -q 'png'
	# two jobs in one process each get their own VA (card run 2026-10-12)
	! $(BUILD)/rdna3_drminfo_host --dma-test --gfx-test 2>&1 | grep -q 'GEM_VA'
	tests/test_oc_install.sh
	tests/test_make_pkg.sh
	tests/test_triage_a3.sh
	python3 -I -B tests/test_accel_hash_parity.py
	python3 -I -B tests/test_trace_parity.py
	tests/test_triage_tx.sh
	tests/test_triage_stale_plugin.sh
	tests/test_triage_latch.sh
	tests/test_result_checklist.sh
	tests/test_triage_trace.sh
	tests/test_triage_vram.sh
	tests/test_plugin_trace.sh

tools: $(BUILD)/amd_discovery_dump $(BUILD)/amd_fw_dump

kext:
	$(MAKE) -C kext

clean:
	rm -rf build
	$(MAKE) -C kext clean 2>/dev/null || true
