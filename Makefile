CC ?= cc
TARGET_CC ?= $(shell if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then echo /opt/homebrew/opt/llvm/bin/clang; elif command -v clang >/dev/null 2>&1; then command -v clang; else echo clang; fi)
ifeq ($(shell if command -v "$(TARGET_CC)" >/dev/null 2>&1 || [ -x "$(TARGET_CC)" ]; then echo yes; else echo no; fi),no)
TARGET_CC := $(shell if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then echo /opt/homebrew/opt/llvm/bin/clang; elif command -v clang >/dev/null 2>&1; then command -v clang; else echo clang; fi)
endif
HOST_OS := $(shell uname -s 2>/dev/null || echo unknown)
HOST_ARCH := $(shell uname -m 2>/dev/null || echo unknown)
TARGET_ARCH ?= $(if $(filter Linux,$(HOST_OS)),$(if $(filter x86_64,$(HOST_ARCH)),x86_64,aarch64),aarch64)
TARGET_ARCH_DIR := src/arch/$(TARGET_ARCH)/linux
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -Isrc/shared -Isrc/compiler -Isrc/platform/posix -Isrc/platform/linux -I$(TARGET_ARCH_DIR)
FREESTANDING_CFLAGS ?= -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
BUILD_DIR ?= build
TARGET_BUILD_DIR ?= build/linux-$(TARGET_ARCH)
ifeq ($(TARGET_ARCH),x86_64)
TARGET_TRIPLE ?= x86_64-linux-none
else ifeq ($(TARGET_ARCH),aarch64)
TARGET_TRIPLE ?= aarch64-linux-none
else
$(error Unsupported TARGET_ARCH '$(TARGET_ARCH)'; expected x86_64 or aarch64)
endif
PARALLEL_JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
PARALLEL_MAKEFLAGS := $(filter -j,$(MAKEFLAGS)) $(filter -j%,$(MAKEFLAGS)) $(filter --jobserver%,$(MAKEFLAGS))
TOOLS := sh ls cat echo pwd mkdir rm rmdir cp mv ln chmod chown uname hostname touch gzip gunzip bzip2 bunzip2 xz unxz tar md5sum sha256sum sha512sum sleep env kill wc head tail ps sort cut tr grep ping id whoami find sed awk date tee xargs dd od hexdump basename dirname realpath cmp diff file strings printf which readlink stat du df netcat ncc man test [ true false expr uniq seq mktemp yes less more patch make tac nl paste join split csplit shuf fold fmt tsort sync truncate timeout expand unexpand printenv ed bc pstree free uptime who users groups column rev
TOOL_SOURCES := $(addprefix src/tools/,$(addsuffix .c,$(TOOLS)))
COMPILER_SOURCES := \
	src/compiler/backend.c \
	src/compiler/backend_expressions.c \
	src/compiler/backend_codegen.c \
	src/compiler/driver.c \
	src/compiler/ir.c \
	src/compiler/object_writer.c \
	src/compiler/parser.c \
	src/compiler/parser_types.c \
	src/compiler/parser_expressions.c \
	src/compiler/parser_declarations.c \
	src/compiler/parser_statements.c \
	src/compiler/preprocessor.c \
	src/compiler/semantic.c \
	src/compiler/source.c \
	src/compiler/lexer.c
COMPILER_IMPL_INCLUDES := \
	src/compiler/backend_internal.h \
	src/compiler/parser_internal.h
SHARED_SOURCES := \
	src/shared/runtime/memory.c \
	src/shared/runtime/string.c \
	src/shared/runtime/parse.c \
	src/shared/runtime/io.c \
	src/shared/tool_util.c \
	src/shared/archive_util.c \
	src/shared/hash_util.c
SHELL_SOURCES := \
	src/shared/shell_parser.c \
	src/shared/shell_execution.c \
	src/shared/shell_builtins.c \
	src/shared/shell_interactive.c
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
TARGET_CRT := $(TARGET_ARCH_DIR)/crt0.S

.DEFAULT_GOAL := all

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

$(BUILD_DIR)/sh: src/tools/sh.c $(SHARED_SOURCES) $(SHELL_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/shell_shared.h $(HOST_PLATFORM_SOURCES) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) $< $(SHARED_SOURCES) $(SHELL_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/sh: src/tools/sh.c $(SHARED_SOURCES) $(SHELL_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/shell_shared.h $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) --target=$(TARGET_TRIPLE) $(CFLAGS) $(FREESTANDING_CFLAGS) -nostdlib -static -fuse-ld=lld $< $(SHARED_SOURCES) $(SHELL_SOURCES) $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) -o $@

$(BUILD_DIR)/ncc: src/tools/ncc.c $(COMPILER_SOURCES) $(COMPILER_IMPL_INCLUDES) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/compiler/backend.h src/compiler/backend_internal.h src/compiler/compiler.h src/compiler/object_writer.h src/compiler/source.h src/compiler/lexer.h src/compiler/ir.h src/compiler/parser.h src/compiler/preprocessor.h src/compiler/semantic.h $(HOST_PLATFORM_SOURCES) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) $< $(COMPILER_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/ncc: src/tools/ncc.c $(COMPILER_SOURCES) $(COMPILER_IMPL_INCLUDES) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/compiler/backend.h src/compiler/backend_internal.h src/compiler/compiler.h src/compiler/object_writer.h src/compiler/source.h src/compiler/lexer.h src/compiler/ir.h src/compiler/parser.h src/compiler/preprocessor.h src/compiler/semantic.h $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) --target=$(TARGET_TRIPLE) $(CFLAGS) $(FREESTANDING_CFLAGS) -nostdlib -static -fuse-ld=lld $< $(COMPILER_SOURCES) $(SHARED_SOURCES) $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) -o $@

$(BUILD_DIR)/%: src/tools/%.c $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) $< $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/%: src/tools/%.c $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) --target=$(TARGET_TRIPLE) $(CFLAGS) $(FREESTANDING_CFLAGS) -nostdlib -static -fuse-ld=lld $< $(SHARED_SOURCES) $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) -o $@

clean:
	rm -rf $(BUILD_DIR) tests/tmp
