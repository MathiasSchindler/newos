ROOT := ../../..
CC ?= cc
TARGET_CC ?= $(CC)
HOST_OS := $(shell uname -s 2>/dev/null || echo unknown)
HOST_ARCH_RAW := $(shell uname -m 2>/dev/null || echo unknown)
HOST_ARCH := $(if $(filter arm64,$(HOST_ARCH_RAW)),aarch64,$(HOST_ARCH_RAW))
LOCAL_MACOS_FREESTANDING := $(if $(filter Darwin,$(HOST_OS)),$(if $(filter aarch64,$(HOST_ARCH)),1,0),0)
TARGET_ARCH ?= $(if $(filter 1,$(LOCAL_MACOS_FREESTANDING)),aarch64,x86_64)
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/.obj/$(if $(filter 1,$(LOCAL_MACOS_FREESTANDING)),macos-$(TARGET_ARCH),linux-$(TARGET_ARCH))
PARALLEL_JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
PARALLEL_MAKEFLAGS := $(filter -j,$(MAKEFLAGS)) $(filter -j%,$(MAKEFLAGS)) $(filter --jobserver%,$(MAKEFLAGS))

ifeq ($(LOCAL_MACOS_FREESTANDING),1)
MACOS_FREESTANDING_CC ?= $(shell xcrun --find clang 2>/dev/null || echo $(CC))
MACOS_FREESTANDING_SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)
MACOS_FREESTANDING_TRIPLE ?= arm64-apple-macos11
ROOT_LINKER ?= $(ROOT)/build/linker
TARGET_CC := $(MACOS_FREESTANDING_CC)
TARGET_ARCH_DIR := $(ROOT)/src/arch/$(TARGET_ARCH)/macos
COMMON_CFLAGS ?= -target $(MACOS_FREESTANDING_TRIPLE) $(if $(MACOS_FREESTANDING_SDKROOT),-isysroot $(MACOS_FREESTANDING_SDKROOT)) -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -fno-common -fno-jump-tables -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions -DNEWOS_MACOS_NEWLINKER -DNEWOS_CRYPTO_CURVE25519_FORCE_64BIT=1 -DNEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES=1 -DNEWOS_TOOL_DEFAULT_COLOR_NEVER=1 -flto -I$(ROOT)/src/shared -I$(ROOT)/src/compiler -I$(ROOT)/src/platform/macos -I$(TARGET_ARCH_DIR)
LDFLAGS ?= --macho-compact --gc-sections
TARGET_CC_TARGET_FLAG ?=
TARGET_BUILTINS_LIB ?=
else
TARGET_ARCH_DIR := $(ROOT)/src/arch/$(TARGET_ARCH)/linux
ifeq ($(TARGET_ARCH),x86_64)
TARGET_TRIPLE ?= x86_64-linux-none
else ifeq ($(TARGET_ARCH),aarch64)
TARGET_TRIPLE ?= aarch64-linux-none
else
$(error Unsupported TARGET_ARCH '$(TARGET_ARCH)'; expected x86_64 or aarch64)
endif
TARGET_CC_TARGET_FLAG ?= $(shell mkdir -p "$(OBJ_DIR)" && printf 'int main(void){return 0;}\n' | "$(TARGET_CC)" --target=$(TARGET_TRIPLE) -x c - -c -o "$(OBJ_DIR)/newos-gitd-target-check.o" >/dev/null 2>&1 && echo --target=$(TARGET_TRIPLE); rm -f "$(OBJ_DIR)/newos-gitd-target-check.o")
TARGET_BUILTINS_LIB ?= $(shell "$(TARGET_CC)" -print-libgcc-file-name >/dev/null 2>&1 && echo -lgcc || true)
COMMON_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fPIE -DNEWOS_DISABLE_STACK_GUARD_INIT=1 -I$(ROOT)/src/shared -I$(ROOT)/src/compiler -I$(ROOT)/src/platform/posix -I$(ROOT)/src/platform/linux -I$(ROOT)/src/platform/common -I$(TARGET_ARCH_DIR)
LDFLAGS ?= -nostdlib -static-pie -Wl,--gc-sections -Wl,-s
endif

SHARED_SOURCES := $(addprefix $(ROOT)/,$(shell cd $(ROOT) && grep -oE '"src/shared/(runtime/[^"]+|compression/[^"]+|tool_[^"]+|archive_util|object_util|archive_zip|bignum|simple_config|server_log|xml|xml_stream|xml_dtd)\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u))
TLS_SOURCES := $(addprefix $(ROOT)/,$(shell cd $(ROOT) && grep -oE '"src/shared/tls/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u))
CRYPTO_SOURCES := $(addprefix $(ROOT)/,$(shell cd $(ROOT) && grep -oE '"src/shared/crypto/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u))
TARGET_PLATFORM_MANIFEST_SOURCES := $(shell cd $(ROOT) && grep -oE '"src/platform/linux/[^"]+\.c"|"src/arch/[^"]+/linux/[^"]+\.(c|S)"' src/compiler/source_manifest.h | tr -d '"')
ifeq ($(LOCAL_MACOS_FREESTANDING),1)
TARGET_PLATFORM_C_SOURCES := $(ROOT)/src/platform/macos/freestanding.c $(ROOT)/src/platform/macos/newlinker_runtime.c $(ROOT)/src/platform/macos/tls.c
TARGET_PLATFORM_ASM_SOURCES :=
TARGET_TLS_PLATFORM_SOURCE :=
TARGET_CRT := $(ROOT)/src/platform/macos/newlinker_start.S
else
TARGET_PLATFORM_SOURCES := $(addprefix $(ROOT)/,$(filter src/platform/linux/% src/arch/$(TARGET_ARCH)/linux/%,$(TARGET_PLATFORM_MANIFEST_SOURCES)))
TARGET_PLATFORM_C_SOURCES := $(filter %.c,$(TARGET_PLATFORM_SOURCES))
TARGET_PLATFORM_ASM_SOURCES := $(filter %.S,$(TARGET_PLATFORM_SOURCES))
TARGET_TLS_PLATFORM_SOURCE := $(ROOT)/src/platform/linux/tls.c
TARGET_CRT := $(TARGET_ARCH_DIR)/crt0.S
endif

FREESTANDING_C_SOURCES := $(TLS_SOURCES) $(CRYPTO_SOURCES) $(TARGET_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) $(TARGET_PLATFORM_C_SOURCES)
FREESTANDING_ASM_SOURCES := $(TARGET_PLATFORM_ASM_SOURCES) $(TARGET_CRT)
GITD_PRIVATE_SOURCES := $(wildcard gitd/*.c)
GITD_PRIVATE_HEADERS := $(wildcard gitd/*.h)
source_object = $(OBJ_DIR)/$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(patsubst $(ROOT)/%,%,$(1))))
FREESTANDING_OBJECTS := $(foreach src,$(FREESTANDING_C_SOURCES) $(FREESTANDING_ASM_SOURCES),$(call source_object,$(src)))
MACOS_CRT_OBJECT := $(call source_object,$(TARGET_CRT))

.PHONY: all freestanding test clean
.SECONDARY: $(OBJ_DIR)/gitd.o

ifeq ($(AUTO_PARALLEL),1)
all: freestanding
else ifneq ($(strip $(PARALLEL_MAKEFLAGS)),)
all: freestanding
else
all:
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) freestanding
endif

freestanding: $(BUILD_DIR)/gitd

test: freestanding
	sh ./smoke.sh

$(BUILD_DIR) $(OBJ_DIR):
	mkdir -p $@

ifeq ($(LOCAL_MACOS_FREESTANDING),1)
$(BUILD_DIR)/gitd: $(OBJ_DIR)/gitd.o $(FREESTANDING_OBJECTS) $(ROOT_LINKER) | $(BUILD_DIR)
	$(ROOT_LINKER) --target=mach-o-arm64 $(LDFLAGS) --lto-cc="$(TARGET_CC)" -o $@ $(MACOS_CRT_OBJECT) $< $(filter-out $(MACOS_CRT_OBJECT),$(FREESTANDING_OBJECTS))

$(ROOT_LINKER):
	$(MAKE) -C $(ROOT) host
else
$(BUILD_DIR)/gitd: $(OBJ_DIR)/gitd.o $(FREESTANDING_OBJECTS) | $(BUILD_DIR)
	$(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) $< $(FREESTANDING_OBJECTS) $(LDFLAGS) $(TARGET_BUILTINS_LIB) -o $@
endif

$(OBJ_DIR)/%.o: %.c Makefile $(ROOT)/src/tools/git.c $(ROOT)/src/tools/git/*.c $(GITD_PRIVATE_SOURCES) $(GITD_PRIVATE_HEADERS) | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(ROOT)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(ROOT)/%.S | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)