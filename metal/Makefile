# Builds the AMD Metal plugin bundle and the AIR->SPIR-V translator (macOS).
#   make VULKAN_HEADERS=/path/to/Vulkan-Headers/include
# Output: ../build/metal/RDNA3MTLDriver.bundle, ../build/metal/librdna3mtl_translate.dylib

BUILD   := ../build/metal
BUNDLE  := $(BUILD)/RDNA3MTLDriver.bundle
VULKAN_HEADERS ?= $(BUILD)/Vulkan-Headers/include
CC      := xcrun clang
RELEASE ?=

.PHONY: all plugin translator clean
all: plugin translator

$(BUILD):
	mkdir -p $@

$(VULKAN_HEADERS)/vulkan/vulkan.h: | $(BUILD)
	git clone -q --depth 1 https://github.com/KhronosGroup/Vulkan-Headers.git $(BUILD)/Vulkan-Headers

plugin: $(VULKAN_HEADERS)/vulkan/vulkan.h | $(BUILD)
	mkdir -p $(BUNDLE)/Contents/MacOS
	cp bundle/Info.plist $(BUNDLE)/Contents/Info.plist
	$(CC) -arch x86_64 -mmacosx-version-min=15.0 -bundle -fobjc-arc -O1 \
	  $(if $(RELEASE),-DNVMTL_RELEASE=1) -Wno-protocol -Wno-incomplete-implementation \
	  -I$(VULKAN_HEADERS) -Iplugin -o $(BUNDLE)/Contents/MacOS/RDNA3MTLDriver plugin/NVMTLDevice.m \
	  -framework Foundation -framework Metal -framework IOSurface 2> $(BUILD)/plugin.log \
	  || { grep -A3 ': error:' $(BUILD)/plugin.log | head -60; exit 1; }
	@echo "warnings: $$(grep -c ': warning:' $(BUILD)/plugin.log)"
	codesign -f -s - $(BUNDLE) 2>/dev/null || true

translator: | $(BUILD)
	cargo build --release --manifest-path translator/wrapper/Cargo.toml --target-dir $(BUILD)/xlate-target
	cp $(BUILD)/xlate-target/release/libnvmtl_translate.dylib $(BUILD)/librdna3mtl_translate.dylib
	install_name_tool -id @rpath/librdna3mtl_translate.dylib $(BUILD)/librdna3mtl_translate.dylib
	codesign -f -s - $(BUILD)/librdna3mtl_translate.dylib 2>/dev/null || true

clean:
	rm -rf $(BUILD)
