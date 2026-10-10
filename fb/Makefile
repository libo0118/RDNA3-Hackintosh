# Builds RDNA3Framebuffer.kext (Stage 3: IOFramebuffer over the scanout the
# firmware left on DCN 3.2) on macOS with the Xcode command line tools.
#   make        -> build/RDNA3Framebuffer.kext
#   make check  -> metaclass names (no NullMoth NV* names may survive)

KEXT     := RDNA3Framebuffer
SDK      ?= $(shell xcrun --sdk macosx --show-sdk-path)
CXX      := xcrun clang++
RESDIR   := $(shell xcrun clang -print-resource-dir)
KHDR     := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers

BUILD    := ../build
OBJDIR   := $(BUILD)/fb-obj
BUNDLE   := $(BUILD)/$(KEXT).kext

ARCH     := -arch x86_64 -mmacosx-version-min=11.0
CXXFLAGS := $(ARCH) -isysroot $(SDK) -mkernel -nostdinc -I$(KHDR) -isystem $(RESDIR)/include \
            -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
            -std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions -fno-builtin -fno-common \
            -O2 -g -ferror-limit=0 -Wall -Wextra -Werror -Wno-unused-parameter \
            -Wno-inconsistent-missing-override -Wno-deprecated-declarations
# the build id is a hash of the sources, not the git commit: macOS asks the
# user to allow every new binary (Auxiliary KC), so the binary may change
# only when the framebuffer itself does (tools/triage_reports.py computes
# the same hash)
FB_SRC_HASH := $(shell cat rdna3-fb.cpp Info.plist ../accel/rdna3k_abi.h | shasum | cut -c1-10)
CXXFLAGS += -DFB_COMMIT='"$(FB_SRC_HASH)"'
LDFLAGS  := $(ARCH) -isysroot $(SDK) -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext

.PHONY: all check clean
all: $(BUNDLE)

$(OBJDIR)/rdna3-fb.o: rdna3-fb.cpp ../accel/rdna3k_abi.h
	mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUNDLE): $(OBJDIR)/rdna3-fb.o Info.plist
	mkdir -p $@/Contents/MacOS
	$(CXX) $(LDFLAGS) $< -o $@/Contents/MacOS/$(KEXT)
	cp Info.plist $@/Contents/Info.plist
	plutil -lint $@/Contents/Info.plist
	@echo "built $@"

check: $(BUNDLE)
	nm -m $(BUNDLE)/Contents/MacOS/$(KEXT) | grep -E "_kmod_info|RDNA3Framebuffer10gMetaClassE"
	@! nm $(BUNDLE)/Contents/MacOS/$(KEXT) | grep -E "__ZN[0-9]+NV[A-Za-z0-9]*10gMetaClassE"

clean:
	rm -rf $(OBJDIR) $(BUNDLE)
