# Builds RDNA3Accelerator.kext (Stage 7: IOGraphicsAccelerator2 subclass
# that makes Metal/WindowServer see the card) on macOS with Xcode CLT.
#   make                 -> build/RDNA3Accelerator.kext
#   make NM_TAHOE=1      -> macOS 26 build (vtable forwarders, tahoe_fwd.h)
# It links against IOAcceleratorFamily2 symbols resolved at load time; the
# class layouts come from the reconstructed headers in re/ and iofam/.

KEXT     := RDNA3Accelerator
SDK      ?= $(shell xcrun --sdk macosx --show-sdk-path)
CXX      := xcrun clang++
RESDIR   := $(shell xcrun clang -print-resource-dir)
KHDR     := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers

BUILD    := ../build
OBJDIR   := $(BUILD)/accel-obj
BUNDLE   := $(BUILD)/$(KEXT).kext

ARCH     := -arch x86_64 -mmacosx-version-min=11.0
CXXFLAGS := $(ARCH) -isysroot $(SDK) -mkernel -nostdinc -I$(KHDR) -isystem $(RESDIR)/include \
            -I. -Iiofam -Ire \
            -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT $(if $(NM_TAHOE),-DNM_TAHOE) \
            -std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions -fno-builtin -fno-common \
            -O2 -g -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override \
            -Wno-unused-function -Wno-unused-variable -Wno-deprecated-declarations
LDFLAGS  := $(ARCH) -isysroot $(SDK) -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext

HDRS     := $(wildcard *.h iofam/*.h re/*.h)

.PHONY: all check clean
all: $(BUNDLE)

$(OBJDIR)/rdna3-accel.o: rdna3-accel.cpp $(HDRS)
	mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUNDLE): $(OBJDIR)/rdna3-accel.o Info.plist
	mkdir -p $@/Contents/MacOS
	$(CXX) $(LDFLAGS) $< -o $@/Contents/MacOS/$(KEXT)
	cp Info.plist $@/Contents/Info.plist
	plutil -lint $@/Contents/Info.plist
	@echo "built $@"

check: $(BUNDLE)
	nm -m $(BUNDLE)/Contents/MacOS/$(KEXT) | grep -E "_kmod_info|RDNA3Accelerator10gMetaClassE"
	@echo "metaclass names (must not collide with any other installed kext):"
	@nm $(BUNDLE)/Contents/MacOS/$(KEXT) | grep -oE "__ZN[0-9]+(RD3|RDNA3|IOAccelRDNA3)[A-Za-z0-9]*10gMetaClassE" | sort -u
	@! nm $(BUNDLE)/Contents/MacOS/$(KEXT) | grep -E "__ZN[0-9]+NV[A-Za-z0-9]*10gMetaClassE"

clean:
	rm -rf $(OBJDIR) $(BUNDLE)
