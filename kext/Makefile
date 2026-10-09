# Builds RDNA3Accel.kext on macOS with the Xcode command line tools.
#   make                 -> build/RDNA3Accel.kext
#   make check           -> kextlibs / plist sanity (does not load anything)
# Loading on the test machine: see docs/STAGE1.md.

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

ARCH     := -arch x86_64 -mmacosx-version-min=11.0
DEFS     := -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
            -DKEXT_VERSION='"$(VERSION)"'
COMMONF  := $(ARCH) -isysroot $(SDK) -mkernel -nostdinc \
            -I$(KHDR) -isystem $(RESDIR)/include -I../common -I../hw -I../uapi -I../third_party/amd -I../accel -Isrc \
            -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter $(DEFS)
CFLAGS   := $(COMMONF) -std=gnu11
CXXFLAGS := $(COMMONF) -std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions \
            -Wno-inconsistent-missing-override -Wno-deprecated-declarations
LDFLAGS  := $(ARCH) -isysroot $(SDK) -nostdlib -Xlinker -kext \
            -lkmod -lkmodc++ -lcc_kext

CXXSRC := src/RDNA3Accel.cpp src/RDNA3Bringup.cpp src/RDNA3Drm.cpp src/RDNA3UserClient.cpp \
          src/RDNA3Platform.cpp src/RDNA3Display.cpp
CSRC   := ../common/amd_discovery.c ../common/amd_fw.c ../common/range_alloc.c \
          ../hw/rdna3_hw.c ../hw/psp13.c ../hw/smu13.c ../hw/gmc11.c ../hw/mmhub302.c ../hw/ih60.c ../hw/sdma60.c ../hw/gpuvm.c ../hw/gfx11.c ../hw/dcn32.c ../hw/doorbell.c ../hw/vbios.c ../hw/dcaux.c ../hw/edid.c ../hw/atom.c ../hw/dmub32.c ../uapi/rdna3_drm.c src/kmod_info.c
HDRS   := $(wildcard src/*.h src/*.hpp ../common/*.h ../hw/*.h ../hw/gen/*.h ../uapi/*.h ../third_party/amd/*.h ../accel/rdna3k_abi.h)
OBJS   := $(patsubst %.cpp,$(OBJDIR)/%.o,$(notdir $(CXXSRC))) \
          $(patsubst %.c,$(OBJDIR)/%.o,$(notdir $(CSRC)))
vpath %.cpp src
vpath %.c src ../common ../hw ../uapi

.PHONY: all check clean firmware

# make firmware FW_DIR=/path/to/linux-firmware/amdgpu
# copies the Navi 32 blobs + LICENSE.amdgpu into the built kext (never into git)
FW_FILES := psp_13_0_7_sos.bin smu_13_0_7.bin sdma_6_0_3.bin gc_11_0_3_imu.bin \
            gc_11_0_3_pfp.bin gc_11_0_3_me.bin gc_11_0_3_mec.bin gc_11_0_3_rlc.bin \
            gc_11_0_3_mes_2.bin gc_11_0_3_mes1.bin dcn_3_2_1_dmcub.bin vcn_4_0_4.bin

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
	@# on Apple Silicon CI runners it reports everything as missing.
	@if [ "$$(uname -m)" = x86_64 ]; then kextlibs -xml $(BUNDLE); fi

firmware: $(BUNDLE)
	@test -n "$(FW_DIR)" || (echo "set FW_DIR=<linux-firmware>/amdgpu" && false)
	mkdir -p $(BUNDLE)/Contents/Resources
	for f in $(FW_FILES); do cp "$(FW_DIR)/$$f" $(BUNDLE)/Contents/Resources/ || exit 1; done
	-cp "$(FW_DIR)/../LICENSE.amdgpu" "$(FW_DIR)/../LICENSES/LICENSE.amdgpu" $(BUNDLE)/Contents/Resources/ 2>/dev/null
	@echo "firmware copied into $(BUNDLE)/Contents/Resources"

clean:
	rm -rf $(OBJDIR) $(BUNDLE)
