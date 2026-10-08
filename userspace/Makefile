# macOS userspace: libdrm_xnu (static + dylib) and the Stage 4 smoke test.
CC      := xcrun clang
CFLAGS  := -O2 -g -Wall -Wextra -Werror -arch x86_64 -mmacosx-version-min=15.0 \
           -Ilibdrm_xnu -I../uapi -I../common -I../third_party/amd
LIBS    := -framework IOKit -framework CoreFoundation
BUILD   := ../build/userspace

.PHONY: all clean
all: $(BUILD)/libdrm_xnu.a $(BUILD)/libdrm_xnu.dylib $(BUILD)/rdna3_drminfo

$(BUILD):
	mkdir -p $@

$(BUILD)/drm_xnu.o: libdrm_xnu/drm_xnu.c libdrm_xnu/drm_xnu.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/libdrm_xnu.a: $(BUILD)/drm_xnu.o
	libtool -static -o $@ $^

$(BUILD)/libdrm_xnu.dylib: $(BUILD)/drm_xnu.o
	$(CC) $(CFLAGS) -dynamiclib -install_name @rpath/libdrm_xnu.dylib -o $@ $^ $(LIBS)

$(BUILD)/rdna3_drminfo: rdna3_drminfo.c $(BUILD)/libdrm_xnu.a
	$(CC) $(CFLAGS) -o $@ $< $(BUILD)/libdrm_xnu.a $(LIBS)

clean:
	rm -rf $(BUILD)
