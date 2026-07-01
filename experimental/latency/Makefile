CC ?= cc
TARGET_CC ?= $(CC)
ROOT := ../..
HOST_OS := $(shell uname -s 2>/dev/null || echo unknown)
HOST_ARCH_RAW := $(shell uname -m 2>/dev/null || echo unknown)
HOST_ARCH := $(if $(filter arm64,$(HOST_ARCH_RAW)),aarch64,$(HOST_ARCH_RAW))
TARGET_ARCH ?= x86_64
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/.obj/linux-$(TARGET_ARCH)

ifeq ($(TARGET_ARCH),x86_64)
TARGET_TRIPLE ?= x86_64-linux-none
else ifeq ($(TARGET_ARCH),aarch64)
TARGET_TRIPLE ?= aarch64-linux-none
else
$(error Unsupported TARGET_ARCH '$(TARGET_ARCH)'; expected x86_64 or aarch64)
endif

TARGET_ARCH_DIR := $(ROOT)/src/arch/$(TARGET_ARCH)/linux
TARGET_CC_TARGET_FLAG ?= $(shell mkdir -p "$(OBJ_DIR)" && printf 'int main(void){return 0;}\n' | "$(TARGET_CC)" --target=$(TARGET_TRIPLE) -x c - -c -o "$(OBJ_DIR)/newos-etherlat-target-check.o" >/dev/null 2>&1 && echo --target=$(TARGET_TRIPLE); rm -f "$(OBJ_DIR)/newos-etherlat-target-check.o")
TARGET_BUILTINS_LIB ?= $(shell "$(TARGET_CC)" -print-libgcc-file-name >/dev/null 2>&1 && echo -lgcc || true)
COMMON_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fPIE -DNEWOS_DISABLE_STACK_GUARD_INIT=1 -I$(ROOT)/src/shared -I$(ROOT)/src/platform/linux -I$(ROOT)/src/platform/common -I$(TARGET_ARCH_DIR)
LDFLAGS ?= -nostdlib -static-pie -Wl,--gc-sections -Wl,-s
BUSY_POLL_US ?= 50

RUNTIME_SOURCES := \
    $(ROOT)/src/shared/runtime/memory.c \
    $(ROOT)/src/shared/runtime/string.c \
    $(ROOT)/src/shared/runtime/parse.c \
    $(ROOT)/src/shared/runtime/io.c

PLATFORM_SOURCES := \
    $(ROOT)/src/platform/linux/io.c \
    $(ROOT)/src/platform/linux/time.c

TARGET_CRT := $(TARGET_ARCH_DIR)/crt0.S
TARGET_PLATFORM_ASM_SOURCES := $(wildcard $(TARGET_ARCH_DIR)/syscall_stubs.S)
FREESTANDING_C_SOURCES := $(RUNTIME_SOURCES) $(PLATFORM_SOURCES)
FREESTANDING_ASM_SOURCES := $(TARGET_PLATFORM_ASM_SOURCES) $(TARGET_CRT)
source_object = $(OBJ_DIR)/$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(patsubst $(ROOT)/%,%,$(1))))
FREESTANDING_OBJECTS := $(foreach src,$(FREESTANDING_C_SOURCES) $(FREESTANDING_ASM_SOURCES),$(call source_object,$(src)))
CRT_OBJECT := $(call source_object,$(TARGET_CRT))

.PHONY: all freestanding test grant-cap test-loopback test-udp-loopback test-etherlat-loopback test-etherlat-discover-loopback test-busy-poll-loopback test-udp-busy-poll-loopback test-etherlat-busy-poll-loopback clean
.SECONDARY:

all: freestanding

freestanding: $(BUILD_DIR)/etherlat $(BUILD_DIR)/udplat

test: freestanding
	$(BUILD_DIR)/etherlat --help >/dev/null
	$(BUILD_DIR)/udplat --help >/dev/null
	sh -n bench-etherlat.sh
	sh -n bench-latency.sh

grant-cap: freestanding
	sudo setcap cap_net_raw+ep $(BUILD_DIR)/etherlat
	getcap $(BUILD_DIR)/etherlat

test-loopback: test-udp-loopback test-etherlat-loopback test-etherlat-discover-loopback

test-busy-poll-loopback: test-udp-busy-poll-loopback test-etherlat-busy-poll-loopback

test-udp-loopback: freestanding
	$(BUILD_DIR)/udplat listen --bind 127.0.0.1 --port 17777 --count 21 --quiet >/tmp/newos-udplat-lo-listen.out 2>/tmp/newos-udplat-lo-listen.err & listener=$$!; \
	$(BUILD_DIR)/udplat ping 127.0.0.1 --port 17777 --count 1 --timeout-ms 500 --quiet >/tmp/newos-udplat-lo-warmup.out 2>/tmp/newos-udplat-lo-warmup.err || true; \
	$(BUILD_DIR)/udplat ping 127.0.0.1 --port 17777 --count 20 --timeout-ms 500 --quiet --samples > $(BUILD_DIR)/udp-loopback.tsv; \
	ping_status=$$?; \
	kill $$listener 2>/dev/null || true; \
	wait $$listener 2>/dev/null; \
	listen_status=$$?; \
	cat /tmp/newos-udplat-lo-warmup.err >&2; \
	cat /tmp/newos-udplat-lo-listen.err >&2; \
	if [ $$listen_status -ne 0 ] && [ $$listen_status -ne 143 ]; then exit $$listen_status; fi; \
	test $$ping_status -eq 0

test-udp-busy-poll-loopback: freestanding
	$(BUILD_DIR)/udplat listen --bind 127.0.0.1 --port 17777 --count 21 --busy-poll-us $(BUSY_POLL_US) --quiet >/tmp/newos-udplat-busy-lo-listen.out 2>/tmp/newos-udplat-busy-lo-listen.err & listener=$$!; \
	$(BUILD_DIR)/udplat ping 127.0.0.1 --port 17777 --count 1 --timeout-ms 500 --busy-poll-us $(BUSY_POLL_US) --quiet >/tmp/newos-udplat-busy-lo-warmup.out 2>/tmp/newos-udplat-busy-lo-warmup.err || true; \
	$(BUILD_DIR)/udplat ping 127.0.0.1 --port 17777 --count 20 --timeout-ms 500 --busy-poll-us $(BUSY_POLL_US) --quiet --samples > $(BUILD_DIR)/udp-busy-poll-loopback.tsv; \
	ping_status=$$?; \
	kill $$listener 2>/dev/null || true; \
	wait $$listener 2>/dev/null; \
	listen_status=$$?; \
	cat /tmp/newos-udplat-busy-lo-warmup.err >&2; \
	cat /tmp/newos-udplat-busy-lo-listen.err >&2; \
	if [ $$listen_status -ne 0 ] && [ $$listen_status -ne 143 ]; then exit $$listen_status; fi; \
	test $$ping_status -eq 0

test-etherlat-loopback: freestanding
	@if [ "$$(id -u)" != 0 ] && { ! command -v getcap >/dev/null 2>&1 || ! getcap $(BUILD_DIR)/etherlat 2>/dev/null | grep -q cap_net_raw; }; then \
		printf '%s\n' 'etherlat: $(BUILD_DIR)/etherlat lacks CAP_NET_RAW; run: make -C experimental/latency grant-cap' >&2; \
		exit 1; \
	fi
	$(BUILD_DIR)/etherlat listen -i lo --count 21 --quiet >/tmp/newos-etherlat-lo-listen.out 2>/tmp/newos-etherlat-lo-listen.err & listener=$$!; \
	$(BUILD_DIR)/etherlat ping -i lo --dst 00:00:00:00:00:00 --count 1 --timeout-ms 500 --quiet >/tmp/newos-etherlat-lo-warmup.out 2>/tmp/newos-etherlat-lo-warmup.err || true; \
	$(BUILD_DIR)/etherlat ping -i lo --dst 00:00:00:00:00:00 --count 20 --timeout-ms 500 --quiet --samples > $(BUILD_DIR)/loopback.tsv; \
	ping_status=$$?; \
	kill $$listener 2>/dev/null || true; \
	wait $$listener 2>/dev/null; \
	listen_status=$$?; \
	cat /tmp/newos-etherlat-lo-warmup.err >&2; \
	cat /tmp/newos-etherlat-lo-listen.err >&2; \
	if [ $$listen_status -ne 0 ] && [ $$listen_status -ne 143 ]; then exit $$listen_status; fi; \
	test $$ping_status -eq 0

test-etherlat-discover-loopback: freestanding
	@if [ "$$(id -u)" != 0 ] && { ! command -v getcap >/dev/null 2>&1 || ! getcap $(BUILD_DIR)/etherlat 2>/dev/null | grep -q cap_net_raw; }; then \
		printf '%s\n' 'etherlat: $(BUILD_DIR)/etherlat lacks CAP_NET_RAW; run: make -C experimental/latency grant-cap' >&2; \
		exit 1; \
	fi
	$(BUILD_DIR)/etherlat listen -i lo --count 1 --quiet >/tmp/newos-etherlat-discover-lo-listen.out 2>/tmp/newos-etherlat-discover-lo-listen.err & listener=$$!; \
	$(BUILD_DIR)/etherlat discover -i lo --timeout-ms 500 --quiet > $(BUILD_DIR)/discover-loopback.txt; \
	discover_status=$$?; \
	wait $$listener 2>/dev/null; \
	listen_status=$$?; \
	cat /tmp/newos-etherlat-discover-lo-listen.err >&2; \
	if [ $$listen_status -ne 0 ] && [ $$listen_status -ne 143 ]; then exit $$listen_status; fi; \
	test $$discover_status -eq 0; \
	grep -Eq '^([0-9a-f][0-9a-f]:){5}[0-9a-f][0-9a-f]$$' $(BUILD_DIR)/discover-loopback.txt

test-etherlat-busy-poll-loopback: freestanding
	@if [ "$$(id -u)" != 0 ] && { ! command -v getcap >/dev/null 2>&1 || ! getcap $(BUILD_DIR)/etherlat 2>/dev/null | grep -q cap_net_raw; }; then \
		printf '%s\n' 'etherlat: $(BUILD_DIR)/etherlat lacks CAP_NET_RAW; run: make -C experimental/latency grant-cap' >&2; \
		exit 1; \
	fi
	$(BUILD_DIR)/etherlat listen -i lo --count 21 --busy-poll-us $(BUSY_POLL_US) --quiet >/tmp/newos-etherlat-busy-lo-listen.out 2>/tmp/newos-etherlat-busy-lo-listen.err & listener=$$!; \
	$(BUILD_DIR)/etherlat ping -i lo --dst 00:00:00:00:00:00 --count 1 --timeout-ms 500 --busy-poll-us $(BUSY_POLL_US) --quiet >/tmp/newos-etherlat-busy-lo-warmup.out 2>/tmp/newos-etherlat-busy-lo-warmup.err || true; \
	$(BUILD_DIR)/etherlat ping -i lo --dst 00:00:00:00:00:00 --count 20 --timeout-ms 500 --busy-poll-us $(BUSY_POLL_US) --quiet --samples > $(BUILD_DIR)/busy-poll-loopback.tsv; \
	ping_status=$$?; \
	kill $$listener 2>/dev/null || true; \
	wait $$listener 2>/dev/null; \
	listen_status=$$?; \
	cat /tmp/newos-etherlat-busy-lo-warmup.err >&2; \
	cat /tmp/newos-etherlat-busy-lo-listen.err >&2; \
	if [ $$listen_status -ne 0 ] && [ $$listen_status -ne 143 ]; then exit $$listen_status; fi; \
	test $$ping_status -eq 0

$(BUILD_DIR) $(OBJ_DIR):
	mkdir -p $@

$(BUILD_DIR)/etherlat: $(OBJ_DIR)/etherlat.o $(FREESTANDING_OBJECTS) | $(BUILD_DIR)
	$(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) $(CRT_OBJECT) $< $(filter-out $(CRT_OBJECT),$(FREESTANDING_OBJECTS)) $(LDFLAGS) $(TARGET_BUILTINS_LIB) -o $@

$(BUILD_DIR)/udplat: $(OBJ_DIR)/udplat.o $(FREESTANDING_OBJECTS) | $(BUILD_DIR)
	$(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) $(CRT_OBJECT) $< $(filter-out $(CRT_OBJECT),$(FREESTANDING_OBJECTS)) $(LDFLAGS) $(TARGET_BUILTINS_LIB) -o $@

$(OBJ_DIR)/%.o: %.c Makefile | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(ROOT)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(ROOT)/%.S | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)