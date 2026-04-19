CC ?= cc
ifeq ($(origin TARGET_CC), undefined)
ifneq ($(origin CC), file)
TARGET_CC := $(CC)
else
TARGET_CC := $(shell if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then echo /opt/homebrew/opt/llvm/bin/clang; elif command -v clang >/dev/null 2>&1; then command -v clang; else echo $(CC); fi)
endif
endif
ifeq ($(shell if command -v "$(TARGET_CC)" >/dev/null 2>&1 || [ -x "$(TARGET_CC)" ]; then echo yes; else echo no; fi),no)
TARGET_CC := $(shell if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then echo /opt/homebrew/opt/llvm/bin/clang; elif command -v "$(CC)" >/dev/null 2>&1; then command -v "$(CC)"; elif command -v clang >/dev/null 2>&1; then command -v clang; else echo clang; fi)
endif
HOST_OS := $(shell uname -s 2>/dev/null || echo unknown)
HOST_ARCH := $(shell uname -m 2>/dev/null || echo unknown)
TARGET_ARCH ?= $(if $(filter Linux,$(HOST_OS)),$(if $(filter x86_64,$(HOST_ARCH)),x86_64,aarch64),aarch64)
TARGET_ARCH_DIR := src/arch/$(TARGET_ARCH)/linux
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -Isrc/shared -Isrc/compiler -Isrc/platform/posix -Isrc/platform/linux -Isrc/platform/common -I$(TARGET_ARCH_DIR)
FREESTANDING_CFLAGS ?= -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
TARGET_CC_TARGET_FLAG ?= $(shell printf 'int main(void){return 0;}\n' | "$(TARGET_CC)" --target=$(TARGET_TRIPLE) -x c - -c -o /tmp/newos-target-check.o >/dev/null 2>&1 && echo --target=$(TARGET_TRIPLE); rm -f /tmp/newos-target-check.o)
TARGET_LINKER_FLAG ?= $(shell printf 'int main(void){return 0;}\n' | "$(TARGET_CC)" $(TARGET_CC_TARGET_FLAG) -fuse-ld=lld -x c - -o /tmp/newos-lld-check >/dev/null 2>&1 && echo -fuse-ld=lld; rm -f /tmp/newos-lld-check)
TARGET_BUILTINS_LIB ?= $(shell "$(TARGET_CC)" -print-libgcc-file-name >/dev/null 2>&1 && echo -lgcc || true)
TARGET_LDFLAGS ?= -nostdlib -static $(TARGET_LINKER_FLAG) $(TARGET_BUILTINS_LIB)
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
PHASE1_JOBS ?= $(PARALLEL_JOBS)
PARALLEL_MAKEFLAGS := $(filter -j,$(MAKEFLAGS)) $(filter -j%,$(MAKEFLAGS)) $(filter --jobserver%,$(MAKEFLAGS))
LOCAL_PLATFORM_ONLY ?= $(if $(filter Darwin,$(HOST_OS)),1,0)
DEFAULT_ALL_TARGETS := host
ifeq ($(LOCAL_PLATFORM_ONLY),0)
DEFAULT_ALL_TARGETS += freestanding
endif
TOOLS := sh ls cat echo pwd mkdir mount umount rm rmdir cp mv ln chmod chown uname hostname init getty dmesg touch gzip gunzip bzip2 bunzip2 xz unxz tar md5sum sha256sum sha512sum sleep env kill wc head tail ps sort cut tr grep ping id whoami find sed awk date tee xargs dd od hexdump basename dirname realpath cmp diff file strings ar readelf objdump strip printf which readlink stat du df netcat ssh ncc man test [ true false expr uniq seq mktemp yes less more watch wget patch make tac nl paste join split csplit shuf fold fmt tsort sync truncate timeout expand unexpand printenv ed bc pstree free uptime who users groups column rev
TOOL_SOURCES := $(addprefix src/tools/,$(addsuffix .c,$(TOOLS)))
COMPILER_SOURCES := $(shell grep -oE '"src/compiler/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"')
COMPILER_IMPL_INCLUDES := \
	src/compiler/backend_internal.h \
	src/compiler/parser_internal.h
SHARED_SOURCES := $(shell grep -oE '"src/shared/(runtime/[^"]+|tool_[^"]+|archive_util|bignum)\.c"' src/compiler/source_manifest.h | tr -d '"')
CRYPTO_SOURCES := $(shell grep -oE '"src/shared/crypto/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"')
HASH_SOURCES := \
	$(shell grep -oE '"src/shared/hash_util\.c"' src/compiler/source_manifest.h | tr -d '"') \
	$(CRYPTO_SOURCES)
SSH_CORE_SOURCES := $(shell grep -oE '"src/tools/ssh/ssh_(core|known_hosts|client[^"]*)\.c"' src/compiler/source_manifest.h | tr -d '"')
SSH_CRYPTO_SOURCES := \
	$(CRYPTO_SOURCES) \
	src/shared/crypto/curve25519.c \
	src/shared/crypto/ed25519.c \
	src/shared/crypto/chacha20_poly1305.c \
	src/shared/crypto/ssh_kdf.c
SHELL_SOURCES := $(shell grep -oE '"src/tools/sh/shell_[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"')
HOST_PLATFORM_SOURCES := $(shell grep -oE '"src/platform/posix/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"')
# Keep this shell extraction comma-free for compatibility with older GNU make on macOS.
TARGET_PLATFORM_MANIFEST_SOURCES := $(shell grep -oE '"src/platform/linux/[^"]+\.c"|"src/arch/[^"]+/linux/[^"]+\.(c|S)"' src/compiler/source_manifest.h | tr -d '"')
TARGET_PLATFORM_SOURCES := $(filter src/platform/linux/% $(TARGET_ARCH_DIR)/%,$(TARGET_PLATFORM_MANIFEST_SOURCES))
TARGET_CRT := $(TARGET_ARCH_DIR)/crt0.S

.DEFAULT_GOAL := all

.PHONY: all host freestanding test test-phase1 test-smoke benchmark clean

test: test-phase1 test-smoke

test-phase1: host
	PHASE1_JOBS=$(PHASE1_JOBS) sh ./tests/phase1/run_phase1_tests.sh

test-smoke: host
	SKIP_PHASE1=1 sh ./tests/run_smoke_tests.sh

benchmark: host
	./tests/benchmarks/run_benchmarks.sh

ifeq ($(AUTO_PARALLEL),1)
all: $(DEFAULT_ALL_TARGETS)
else ifneq ($(strip $(PARALLEL_MAKEFLAGS)),)
all: $(DEFAULT_ALL_TARGETS)
else
all:
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) $(DEFAULT_ALL_TARGETS)
endif

ifeq ($(AUTO_PARALLEL),1)
host: $(BUILD_DIR)/.ssh_core_check $(addprefix $(BUILD_DIR)/,$(TOOLS))
ifeq ($(LOCAL_PLATFORM_ONLY),1)
freestanding: host
else
freestanding: $(TARGET_BUILD_DIR)/.ssh_core_check $(addprefix $(TARGET_BUILD_DIR)/,$(TOOLS))
endif
else ifneq ($(strip $(PARALLEL_MAKEFLAGS)),)
host: $(BUILD_DIR)/.ssh_core_check $(addprefix $(BUILD_DIR)/,$(TOOLS))
ifeq ($(LOCAL_PLATFORM_ONLY),1)
freestanding: host
else
freestanding: $(TARGET_BUILD_DIR)/.ssh_core_check $(addprefix $(TARGET_BUILD_DIR)/,$(TOOLS))
endif
else
host:
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) host
freestanding:
ifeq ($(LOCAL_PLATFORM_ONLY),1)
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) host
else
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) freestanding
endif
endif

$(BUILD_DIR) $(TARGET_BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/.ssh_core_check: $(SSH_CORE_SOURCES) src/tools/ssh/ssh_core.h src/tools/ssh/ssh_known_hosts.h src/tools/ssh/ssh_client.h src/tools/ssh/ssh_client_internal.h src/shared/platform.h src/shared/runtime.h src/shared/hash_util.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) -fsyntax-only $(SSH_CORE_SOURCES) && : > $@

$(TARGET_BUILD_DIR)/.ssh_core_check: $(SSH_CORE_SOURCES) src/tools/ssh/ssh_core.h src/tools/ssh/ssh_known_hosts.h src/tools/ssh/ssh_client.h src/tools/ssh/ssh_client_internal.h src/shared/platform.h src/shared/runtime.h src/shared/hash_util.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) -fsyntax-only $(SSH_CORE_SOURCES) && : > $@

$(BUILD_DIR)/sh: src/tools/sh.c $(SHARED_SOURCES) $(SHELL_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/sh/shell_shared.h $(HOST_PLATFORM_SOURCES) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) $< $(SHARED_SOURCES) $(SHELL_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/sh: src/tools/sh.c $(SHARED_SOURCES) $(SHELL_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/sh/shell_shared.h $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(SHARED_SOURCES) $(SHELL_SOURCES) $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/ncc: src/tools/ncc.c $(COMPILER_SOURCES) $(COMPILER_IMPL_INCLUDES) src/compiler/source_manifest.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/compiler/backend.h src/compiler/backend_internal.h src/compiler/compiler.h src/compiler/object_writer.h src/compiler/source.h src/compiler/lexer.h src/compiler/ir.h src/compiler/parser.h src/compiler/preprocessor.h src/compiler/semantic.h $(HOST_PLATFORM_SOURCES) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) $< $(COMPILER_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/ncc: src/tools/ncc.c $(COMPILER_SOURCES) $(COMPILER_IMPL_INCLUDES) src/compiler/source_manifest.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/compiler/backend.h src/compiler/backend_internal.h src/compiler/compiler.h src/compiler/object_writer.h src/compiler/source.h src/compiler/lexer.h src/compiler/ir.h src/compiler/parser.h src/compiler/preprocessor.h src/compiler/semantic.h $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(COMPILER_SOURCES) $(SHARED_SOURCES) $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/md5sum $(BUILD_DIR)/sha256sum $(BUILD_DIR)/sha512sum: $(BUILD_DIR)/%: src/tools/%.c $(SHARED_SOURCES) $(HASH_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/hash_util.h src/shared/crypto/crypto_util.h src/shared/crypto/md5.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h $(HOST_PLATFORM_SOURCES) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) $< $(SHARED_SOURCES) $(HASH_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/md5sum $(TARGET_BUILD_DIR)/sha256sum $(TARGET_BUILD_DIR)/sha512sum: $(TARGET_BUILD_DIR)/%: src/tools/%.c $(SHARED_SOURCES) $(HASH_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/hash_util.h src/shared/crypto/crypto_util.h src/shared/crypto/md5.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(SHARED_SOURCES) $(HASH_SOURCES) $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/ssh: src/tools/ssh.c $(SHARED_SOURCES) $(SSH_CORE_SOURCES) $(SSH_CRYPTO_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/ssh/ssh_core.h src/tools/ssh/ssh_known_hosts.h src/tools/ssh/ssh_client.h src/tools/ssh/ssh_client_internal.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h $(HOST_PLATFORM_SOURCES) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) $< $(SHARED_SOURCES) $(SSH_CORE_SOURCES) $(SSH_CRYPTO_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/ssh: src/tools/ssh.c $(SHARED_SOURCES) $(SSH_CORE_SOURCES) $(SSH_CRYPTO_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/ssh/ssh_core.h src/tools/ssh/ssh_known_hosts.h src/tools/ssh/ssh_client.h src/tools/ssh/ssh_client_internal.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(SHARED_SOURCES) $(SSH_CORE_SOURCES) $(SSH_CRYPTO_SOURCES) $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

MAKE_TOOL_SOURCES := src/tools/make/make_parse.c src/tools/make/make_exec.c
AWK_TOOL_SOURCES  := src/tools/awk/awk_parse.c src/tools/awk/awk_exec.c

$(BUILD_DIR)/make: src/tools/make.c $(MAKE_TOOL_SOURCES) src/tools/make/make_impl.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) src/tools/make.c $(MAKE_TOOL_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/make: src/tools/make.c $(MAKE_TOOL_SOURCES) src/tools/make/make_impl.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) src/tools/make.c $(MAKE_TOOL_SOURCES) $(SHARED_SOURCES) $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/awk: src/tools/awk.c $(AWK_TOOL_SOURCES) src/tools/awk/awk_impl.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) src/tools/awk.c $(AWK_TOOL_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/awk: src/tools/awk.c $(AWK_TOOL_SOURCES) src/tools/awk/awk_impl.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) src/tools/awk.c $(AWK_TOOL_SOURCES) $(SHARED_SOURCES) $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/bc: src/tools/bc.c $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) -Wno-pedantic $< $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/bc: src/tools/bc.c $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) -Wno-pedantic $(FREESTANDING_CFLAGS) $< $(SHARED_SOURCES) $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/%: src/tools/%.c $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) $< $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/%: src/tools/%.c $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(SHARED_SOURCES) $(TARGET_PLATFORM_SOURCES) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

clean:
	rm -rf $(BUILD_DIR) tests/tmp
