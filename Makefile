CC ?= cc
TARGET_CC ?= /opt/homebrew/opt/llvm/bin/clang
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -Isrc/shared -Isrc/platform/posix -Isrc/platform/linux -Isrc/arch/aarch64/linux
FREESTANDING_CFLAGS ?= -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
BUILD_DIR ?= build
TARGET_BUILD_DIR ?= build/linux-aarch64
TARGET_TRIPLE ?= aarch64-linux-none
PARALLEL_JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
PARALLEL_MAKEFLAGS := $(filter -j,$(MAKEFLAGS)) $(filter -j%,$(MAKEFLAGS)) $(filter --jobserver%,$(MAKEFLAGS))
TOOLS := sh ls cat echo pwd mkdir rm rmdir cp mv ln chmod uname touch gzip gunzip bzip2 bunzip2 xz unxz tar md5sum sha256sum sha512sum sleep wc head tail ps sort cut tr grep ping id whoami find sed awk date tee xargs dd od hexdump basename dirname realpath cmp diff file strings printf which readlink stat du df
TOOL_SOURCES := $(addprefix src/tools/,$(addsuffix .c,$(TOOLS)))
SHARED_SOURCES := \
	src/shared/runtime/memory.c \
	src/shared/runtime/string.c \
	src/shared/runtime/parse.c \
	src/shared/runtime/io.c \
	src/shared/tool_util.c \
	src/shared/archive_util.c \
	src/shared/hash_util.c
HOST_PLATFORM_SOURCES := \
	src/platform/posix/fs.c \
	src/platform/posix/process.c \
	src/platform/posix/identity.c \
	src/platform/posix/net.c \
	src/platform/posix/time.c
TARGET_PLATFORM_SOURCES := \
	src/platform/linux/fs.c \
	src/platform/linux/process.c \
	src/platform/linux/identity.c \
	src/platform/linux/net.c \
	src/platform/linux/time.c
TARGET_CRT := src/arch/aarch64/linux/crt0.S

.PHONY: all host freestanding test benchmark clean

test: host
	./tests/run_smoke_tests.sh

benchmark: host
	./tests/benchmarks/run_benchmarks.sh

ifeq ($(AUTO_PARALLEL),1)
all: host freestanding
else ifneq ($(strip $(PARALLEL_MAKEFLAGS)),)
all: host freestanding
else
all:
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) host freestanding
endif

ifeq ($(AUTO_PARALLEL),1)
host: $(addprefix $(BUILD_DIR)/,$(TOOLS))
freestanding: $(addprefix $(TARGET_BUILD_DIR)/,$(TOOLS))
else ifneq ($(strip $(PARALLEL_MAKEFLAGS)),)
host: $(addprefix $(BUILD_DIR)/,$(TOOLS))
freestanding: $(addprefix $(TARGET_BUILD_DIR)/,$(TOOLS))
else
host freestanding:
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) $@
endif

$(BUILD_DIR) $(TARGET_BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%: src/tools/%.c $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/%: src/tools/%.c $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) src/arch/aarch64/linux/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	$(TARGET_CC) --target=$(TARGET_TRIPLE) $(CFLAGS) $(FREESTANDING_CFLAGS) -nostdlib -static -fuse-ld=lld $< $(SHARED_SOURCES) $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) -o $@

clean:
	rm -rf $(BUILD_DIR) tests/tmp
