# macOS userspace (Sequoia, x86_64):
#   libdrm.dylib        - xf86drm_xnu + drm_xnu + IOKit backend (libdrm core API subset)
#   libdrm_amdgpu.dylib - unmodified libdrm_amdgpu (third_party/libdrm) on top of it
#   include/ + pkgconfig/ so Mesa's meson finds them as "libdrm" / "libdrm_amdgpu"
#   rdna3_drminfo       - Stage 4 smoke test against the kext
CC      := xcrun clang
ARCH    := -arch x86_64 -mmacosx-version-min=15.0
BUILD   := $(abspath ../build/userspace)
PREFIX  ?= $(BUILD)/prefix
LIBDRM  := ../third_party/libdrm
XNU     := libdrm_xnu
CFLAGS  := -O2 -g $(ARCH) -fPIC -fvisibility=default
WARN    := -Wall -Wextra -Werror
INC_DRM := -I$(XNU)/override -I$(XNU) -I$(LIBDRM) -I$(LIBDRM)/amdgpu -I$(LIBDRM)/include/drm
LIBS    := -framework IOKit -framework CoreFoundation
VERSION := 2.4.134

CORE_OBJS := $(BUILD)/obj/drm_xnu.o $(BUILD)/obj/xf86drm_xnu.o $(BUILD)/obj/backend_iokit.o
AMD_SRC   := $(wildcard $(LIBDRM)/amdgpu/*.c)
AMD_OBJS  := $(patsubst $(LIBDRM)/amdgpu/%.c,$(BUILD)/obj/amdgpu_%.o,$(AMD_SRC))

.PHONY: all install clean
all: $(BUILD)/libdrm.dylib $(BUILD)/libdrm_amdgpu.dylib $(BUILD)/rdna3_drminfo install

$(BUILD)/obj:
	mkdir -p $@

$(BUILD)/obj/%.o: $(XNU)/%.c $(wildcard $(XNU)/*.h) | $(BUILD)/obj
	$(CC) $(CFLAGS) $(WARN) $(INC_DRM) -c $< -o $@

$(BUILD)/obj/amdgpu_%.o: $(LIBDRM)/amdgpu/%.c | $(BUILD)/obj
	$(CC) $(CFLAGS) -w $(INC_DRM) -DHAVE_LIBDRM_ATOMIC_PRIMITIVES=1 \
	  -DAMDGPU_ASIC_ID_TABLE='"/Library/GPUBundles/rdna3mtl/amdgpu.ids"' -c $< -o $@

$(BUILD)/libdrm.dylib: $(CORE_OBJS)
	$(CC) $(ARCH) -dynamiclib -install_name @rpath/libdrm.2.dylib \
	  -current_version 2.4.134 -compatibility_version 2.0.0 -o $@ $^ $(LIBS)

$(BUILD)/libdrm_amdgpu.dylib: $(AMD_OBJS) $(BUILD)/libdrm.dylib
	$(CC) $(ARCH) -dynamiclib -install_name @rpath/libdrm_amdgpu.1.dylib \
	  -o $@ $(AMD_OBJS) -L$(BUILD) -ldrm $(LIBS)

$(BUILD)/rdna3_drminfo: rdna3_drminfo.c $(BUILD)/libdrm.dylib
	$(CC) -O2 -g $(ARCH) $(WARN) -I$(XNU) -I../uapi -I../common -I../third_party/amd \
	  -o $@ $< -L$(BUILD) -ldrm -Wl,-rpath,@executable_path $(LIBS)

install: $(BUILD)/libdrm.dylib $(BUILD)/libdrm_amdgpu.dylib
	mkdir -p $(PREFIX)/lib/pkgconfig $(PREFIX)/include/libdrm
	cp $(BUILD)/libdrm.dylib $(PREFIX)/lib/libdrm.2.dylib
	cp $(BUILD)/libdrm_amdgpu.dylib $(PREFIX)/lib/libdrm_amdgpu.1.dylib
	ln -sf libdrm.2.dylib $(PREFIX)/lib/libdrm.dylib
	ln -sf libdrm_amdgpu.1.dylib $(PREFIX)/lib/libdrm_amdgpu.dylib
	cp $(LIBDRM)/xf86drm.h $(LIBDRM)/libsync.h $(XNU)/drm_xnu.h $(PREFIX)/include/
	cp $(LIBDRM)/include/drm/*.h $(LIBDRM)/amdgpu/amdgpu.h $(PREFIX)/include/libdrm/
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libdrm\nDescription: libdrm core subset over RDNA3Accel (drm_xnu)\nVersion: %s\nLibs: -L$${libdir} -ldrm\nCflags: -I$${includedir} -I$${includedir}/libdrm\n' \
	  "$(PREFIX)" "$(VERSION)" > $(PREFIX)/lib/pkgconfig/libdrm.pc
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libdrm_amdgpu\nDescription: libdrm_amdgpu over drm_xnu\nVersion: %s\nRequires.private: libdrm\nLibs: -L$${libdir} -ldrm_amdgpu\nCflags: -I$${includedir} -I$${includedir}/libdrm\n' \
	  "$(PREFIX)" "$(VERSION)" > $(PREFIX)/lib/pkgconfig/libdrm_amdgpu.pc

clean:
	rm -rf $(BUILD)
