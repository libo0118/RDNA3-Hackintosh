# Builds RDNA3AGDC.kext (the AppleGraphicsDeviceControl object IOPresentment
# asks about the display topology) on macOS with the Xcode command line tools.
#   make        -> build/RDNA3AGDC.kext
#   make check  -> metaclass names, the symbols it takes from Apple's kexts

KEXT     := RDNA3AGDC
SDK      ?= $(shell xcrun --sdk macosx --show-sdk-path)
CXX      := xcrun clang++
RESDIR   := $(shell xcrun clang -print-resource-dir)
KHDR     := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers

BUILD    := ../build
OBJDIR   := $(BUILD)/agdc-obj
BUNDLE   := $(BUILD)/$(KEXT).kext

ARCH     := -arch x86_64 -mmacosx-version-min=11.0
CXXFLAGS := $(ARCH) -isysroot $(SDK) -mkernel -nostdinc -I$(KHDR) -isystem $(RESDIR)/include \
            -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
            -std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions -fno-builtin -fno-common \
            -O2 -g -ferror-limit=0 -Wall -Wextra -Werror -Wno-unused-parameter \
            -Wno-inconsistent-missing-override -Wno-deprecated-declarations
# the build id is a hash of the sources (like fb/ and accel/): macOS asks the
# user to allow every new binary; tools/triage_reports.py computes the same
AGDC_SRC_HASH := $(shell cat rdna3-agdc.cpp rdna3_agdc_k5.h rdna3_agdc_n48.h AppleGraphicsDeviceControl_re.h Info.plist ../common/rdna3_bootargs.h | shasum | cut -c1-10)
CXXFLAGS += -DAGDC_SRC='"$(AGDC_SRC_HASH)"'
LDFLAGS  := $(ARCH) -isysroot $(SDK) -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext

.PHONY: all check clean
all: $(BUNDLE)

$(OBJDIR)/rdna3-agdc.o: rdna3-agdc.cpp rdna3_agdc_k5.h rdna3_agdc_n48.h AppleGraphicsDeviceControl_re.h ../common/rdna3_bootargs.h
	mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUNDLE): $(OBJDIR)/rdna3-agdc.o Info.plist
	mkdir -p $@/Contents/MacOS
	$(CXX) $(LDFLAGS) $< -o $@/Contents/MacOS/$(KEXT)
	cp Info.plist $@/Contents/Info.plist
	plutil -insert RDNA3SourceHash -string $(AGDC_SRC_HASH) $@/Contents/Info.plist
	plutil -lint $@/Contents/Info.plist
	@echo "built $@"

check: $(BUNDLE)
	nm -m $(BUNDLE)/Contents/MacOS/$(KEXT) | grep -E "_kmod_info|RDNA3AGDC10gMetaClassE|RD3DeviceControl10gMetaClassE"
	@! nm $(BUNDLE)/Contents/MacOS/$(KEXT) | grep -E "__ZN[0-9]+NV[A-Za-z0-9]*10gMetaClassE"
	@echo "symbols from AppleGraphicsDeviceControl (must exist in the macOS it loads on):"
	@nm -u $(BUNDLE)/Contents/MacOS/$(KEXT) | grep AppleGraphicsDeviceControl
	@# where the running macOS has the kext: the symbols it does not export
	@# would stop RDNA3AGDC loading (only this kext; the display stays)
	@k=/System/Library/Extensions/AppleGraphicsControl.kext/Contents/PlugIns/AppleGraphicsDeviceControl.kext/Contents/MacOS/AppleGraphicsDeviceControl; \
	if [ -f "$$k" ]; then \
	  miss=$$(nm -u $(BUNDLE)/Contents/MacOS/$(KEXT) | grep AppleGraphicsDeviceControl | while read s; do \
	    nm -g "$$k" 2>/dev/null | grep -q " $$s$$" || echo "$$s"; done); \
	  if [ -n "$$miss" ]; then echo "NOT exported by $$k:"; echo "$$miss"; else echo "all exported by $$k"; fi; \
	else echo "(no $$k on this machine: not checked)"; fi

clean:
	rm -rf $(OBJDIR) $(BUNDLE)
