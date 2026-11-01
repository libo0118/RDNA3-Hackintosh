# Builds RDNA3Accel.kext on macOS with the Xcode command line tools.
#   make                 -> build/RDNA3Accel.kext
#   make check           -> kextlibs / plist sanity (does not load anything)
# Loading on the test machine: see docs/STAGE1.html.

KEXT     := RDNA3Accel
VERSION  := $(shell /usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' Info.plist 2>/dev/null || echo 0.1.0)
SDK      ?= $(shell xcrun --sdk macosx --show-sdk-path)
CC       := xcrun clang
CXX      := xcrun clang++
RESDIR   := $(shell xcrun clang -print-resource-dir)
KHDR     := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers

BUILD    := ../build
OBJDIR   := $(BUILD)/kext-obj
BUNDLE   := $(BUILD)/$(KEXT).kext

GIT_COMMIT ?= $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
ARCH     := -arch x86_64 -mmacosx-version-min=11.0
DEFS     := -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
            -DKEXT_VERSION='"$(VERSION)"' -DKEXT_COMMIT='"$(GIT_COMMIT)"'
COMMONF  := $(ARCH) -isysroot $(SDK) -mkernel -nostdinc \
            -I$(KHDR) -isystem $(RESDIR)/include -I../common -I../hw -I../uapi -I../third_party/amd -I../accel -Isrc \
            -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter $(DEFS)
CFLAGS   := $(COMMONF) -std=gnu11
CXXFLAGS := $(COMMONF) -std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions \
            -Wno-inconsistent-missing-override -Wno-deprecated-declarations
LDFLAGS  := $(ARCH) -isysroot $(SDK) -nostdlib -Xlinker -kext \
            -lkmod -lkmodc++ -lcc_kext

CXXSRC := src/RDNA3Accel.cpp src/RDNA3Bringup.cpp src/RDNA3Drm.cpp src/RDNA3UserClient.cpp \
          src/RDNA3Platform.cpp src/RDNA3Display.cpp src/RDNA3Resume.cpp src/RDNA3Evict.cpp
CSRC   := ../common/amd_discovery.c ../common/amd_fw.c ../common/range_alloc.c \
          ../hw/rdna3_hw.c ../hw/psp13.c ../hw/smu13.c ../hw/gmc11.c ../hw/mmhub302.c ../hw/ih60.c ../hw/sdma60.c ../hw/gpuvm.c ../hw/gfx11.c ../hw/dcn32.c ../hw/doorbell.c ../hw/vbios.c ../hw/dcaux.c ../hw/edid.c ../hw/atom.c ../hw/dmub32.c ../hw/dcn32_mode.c ../hw/dcn32_clone.c ../hw/dcn32_modeset.c ../hw/dcn32_clk.c ../hw/gfx11_info.c ../hw/xfer.c ../hw/soc21_cg.c ../uapi/rdna3_drm.c src/kmod_info.c
CXXSRC += src/RDNA3Hotplug.cpp
CXXSRC += src/RDNA3DisplayPower.cpp
CXXSRC += src/RDNA3DisplayIrq.cpp
CSRC   += ../hw/dchpd.c
CSRC   += ../hw/dcirq.c
CSRC   += ../hw/dcn32_gamma.c
CSRC   += ../hw/dcn32_state.c
CSRC   += ../hw/dcn32_dp.c
CSRC   += ../hw/vcn40.c
CSRC   += ../hw/dcn32_dsc.c ../hw/dp_mst.c ../hw/dcn32_dp_mst.c
CSRC   += ../hw/dcn32_otg.c ../hw/dcn32_color.c ../hw/dcn32_hdmi.c ../hw/dcn32_hwseq.c
CSRC   += ../hw/dcn32_hpo.c ../hw/dcn32_odm.c
CSRC   += ../hw/dcn32_audio.c ../hw/dcn32_opp.c
CSRC   += ../hw/dcn32_pstate.c ../hw/dcn32_subvp.c ../hw/dcn32_freesync.c
CSRC   += ../hw/dcn32_txstate.c
HDRS   := $(wildcard src/*.h src/*.hpp ../common/*.h ../hw/*.h ../hw/gen/*.h ../uapi/*.h ../third_party/amd/*.h ../accel/rdna3k_abi.h)
# a hash of the sources (like fb/ and accel/): the VM's report skips a test
# that passed before with the same build of what it tests
CORE_SRC_HASH := $(shell cat $(CXXSRC) $(CSRC) $(HDRS) Info.plist | shasum | cut -c1-10)
CXXFLAGS += -DKEXT_SRC_HASH='"$(CORE_SRC_HASH)"'
OBJS   := $(patsubst %.cpp,$(OBJDIR)/%.o,$(notdir $(CXXSRC))) \
          $(patsubst %.c,$(OBJDIR)/%.o,$(notdir $(CSRC)))
vpath %.cpp src
vpath %.c src ../common ../hw ../uapi

.PHONY: all check clean firmware

# make firmware FW_DIR=/path/to/linux-firmware/amdgpu
# copies the Navi 32 blobs + LICENSE.amdgpu into the built kext (never into git)
FW_FILES := psp_13_0_10_sos.bin psp_13_0_10_ta.bin smu_13_0_10.bin sdma_6_0_3.bin gc_11_0_3_imu.bin \
            gc_11_0_3_pfp.bin gc_11_0_3_me.bin gc_11_0_3_mec.bin gc_11_0_3_rlc.bin \
            gc_11_0_3_mes_2.bin gc_11_0_3_mes1.bin dcn_3_2_0_dmcub.bin vcn_4_0_0.bin

all: $(BUNDLE)

$(OBJDIR):
	mkdir -p $@

$(OBJDIR)/%.o: %.cpp $(HDRS) | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.c $(HDRS) | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUNDLE): $(OBJS) Info.plist
	mkdir -p $@/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OBJS) -o $@/Contents/MacOS/$(KEXT)
	cp Info.plist $@/Contents/Info.plist
	plutil -lint $@/Contents/Info.plist
	@echo "built $@"

check: $(BUNDLE)
	plutil -lint Info.plist
	nm -m $(BUNDLE)/Contents/MacOS/$(KEXT) | grep -E "_kmod_info|RDNA3Accel10gMetaClassE"
	@echo "undefined symbols (must all come from the OSBundleLibraries in Info.plist):"
	@nm -u $(BUNDLE)/Contents/MacOS/$(KEXT)
	@# kextlibs can only resolve x86_64 KPIs on an Intel Mac / Hackintosh,
	@# on Apple Silicon it reports everything as missing. On macOS 11+
	@# without a KDK it can miss every KPI on Intel too: a report where not
	@# one symbol resolved says nothing about the kext and does not fail.
	@if [ "$$(uname -m)" = x86_64 ]; then \
	  out=$$(kextlibs -xml $(BUNDLE) 2>&1) && { echo "$$out"; exit 0; }; echo "$$out"; \
	  total=$$(nm -u $(BUNDLE)/Contents/MacOS/$(KEXT) | wc -l | tr -d ' '); \
	  miss=$$(echo "$$out" | sed -n 's/^ *\([0-9][0-9]*\) symbols* not found.*/\1/p' | head -1); \
	  if [ -n "$$miss" ] && [ "$$miss" = "$$total" ]; then \
	    echo "kextlibs resolved none of the $$total symbols (no KPI visible on this host): not counted"; \
	  else exit 1; fi; \
	fi

firmware: $(BUNDLE)
	@test -n "$(FW_DIR)" || (echo "set FW_DIR=<linux-firmware>/amdgpu" && false)
	mkdir -p $(BUNDLE)/Contents/Resources
	for f in $(FW_FILES); do cp "$(FW_DIR)/$$f" $(BUNDLE)/Contents/Resources/ || exit 1; done
	-cp "$(FW_DIR)/../LICENSE.amdgpu" "$(FW_DIR)/../LICENSES/LICENSE.amdgpu" $(BUNDLE)/Contents/Resources/ 2>/dev/null
	@echo "firmware copied into $(BUNDLE)/Contents/Resources"

clean:
	rm -rf $(OBJDIR) $(BUNDLE)
