# Host-side build: portable code shared with the kext, its tests and tools.
# The kext itself is built on macOS: see kext/Makefile.

CC      ?= cc
CFLAGS  ?= -O1 -g
WARN    := -std=c99 -Wall -Wextra -Werror -Wshadow -Wcast-align -Wconversion -Wno-sign-conversion
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
INC     := -Icommon -Ithird_party/amd
BUILD   := build/host

COMMON  := common/amd_discovery.c common/amd_fw.c
COMMON_H := $(wildcard common/*.h)

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

# RDNA3_FW_DIR=<linux-firmware/amdgpu> additionally checks real Navi 32 files
test: $(BUILD)/test_discovery $(BUILD)/test_fw
	$(BUILD)/test_discovery
	$(BUILD)/test_fw $(RDNA3_FW_DIR)

tools: $(BUILD)/amd_discovery_dump $(BUILD)/amd_fw_dump

kext:
	$(MAKE) -C kext

clean:
	rm -rf build
	$(MAKE) -C kext clean 2>/dev/null || true
