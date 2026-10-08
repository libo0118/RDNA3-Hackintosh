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
            -I$(KHDR) -isystem $(RESDIR)/include -I../common -Isrc \
            -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter $(DEFS)
CFLAGS   := $(COMMONF) -std=gnu11
CXXFLAGS := $(COMMONF) -std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions \
            -Wno-inconsistent-missing-override -Wno-deprecated-declarations
LDFLAGS  := $(ARCH) -isysroot $(SDK) -nostdlib -Xlinker -kext \
            -lkmod -lkmodc++ -lcc_kext

OBJS := $(OBJDIR)/RDNA3Accel.o $(OBJDIR)/amd_discovery.o $(OBJDIR)/kmod_info.o

.PHONY: all check clean

all: $(BUNDLE)

$(OBJDIR):
	mkdir -p $@

$(OBJDIR)/RDNA3Accel.o: src/RDNA3Accel.cpp src/RDNA3Accel.hpp src/RDNA3Regs.h ../common/amd_discovery.h | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/amd_discovery.o: ../common/amd_discovery.c ../common/amd_discovery.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/kmod_info.o: src/kmod_info.c | $(OBJDIR)
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
	-kextlibs -xml $(BUNDLE)

clean:
	rm -rf $(OBJDIR) $(BUNDLE)
