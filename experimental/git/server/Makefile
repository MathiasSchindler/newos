ROOT := ../../..
CC ?= cc
TARGET_CC ?= $(CC)
HOST_OS := $(shell uname -s 2>/dev/null || echo unknown)
HOST_ARCH_RAW := $(shell uname -m 2>/dev/null || echo unknown)
HOST_ARCH := $(if $(filter arm64,$(HOST_ARCH_RAW)),aarch64,$(HOST_ARCH_RAW))
HOST_OS_NAME := $(if $(filter Darwin,$(HOST_OS)),macos,$(shell printf '%s' "$(HOST_OS)" | tr A-Z a-z))
LOCAL_MACOS_FREESTANDING := $(if $(filter Darwin,$(HOST_OS)),$(if $(filter aarch64,$(HOST_ARCH)),1,0),0)
TARGET_ARCH ?= $(if $(filter 1,$(LOCAL_MACOS_FREESTANDING)),aarch64,x86_64)
BUILD_DIR := build
PROFILE ?= 0
LINKER_REPORTS ?= 0
PROFILE_CFLAGS := $(if $(filter 1 yes true,$(PROFILE)),-finstrument-functions -fno-omit-frame-pointer -fno-inline)
STRIP_LDFLAGS := $(if $(filter 1 yes true,$(PROFILE)),,-Wl,-s)
PROFILE_OBJ_SUFFIX := $(if $(filter 1 yes true,$(PROFILE)),-profile)
OBJ_DIR := $(BUILD_DIR)/.obj/$(if $(filter 1,$(LOCAL_MACOS_FREESTANDING)),macos-$(TARGET_ARCH),linux-$(TARGET_ARCH))$(PROFILE_OBJ_SUFFIX)
MAP_DIR := $(BUILD_DIR)/.maps
PARALLEL_JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
PARALLEL_MAKEFLAGS := $(filter -j,$(MAKEFLAGS)) $(filter -j%,$(MAKEFLAGS)) $(filter --jobserver%,$(MAKEFLAGS))
LINKER_REPORT_FLAGS := $(if $(filter 1 yes true,$(LINKER_REPORTS)),--stats --map $(MAP_DIR)/gitd.map)
ROOT_LINKER_TARGET ?= build/host-$(HOST_OS_NAME)-$(HOST_ARCH)/linker
ROOT_LINKER ?= $(ROOT)/$(ROOT_LINKER_TARGET)
FREESTANDING_USE_NEWLINKER ?= $(if $(filter Linux,$(HOST_OS)),$(if $(filter x86_64,$(TARGET_ARCH)),1,0),0)
NEWLINKER_FLAGS ?= --tiny --gc-sections --icf=safe

ifeq ($(LOCAL_MACOS_FREESTANDING),1)
MACOS_FREESTANDING_CC ?= $(shell xcrun --find clang 2>/dev/null || echo $(CC))
MACOS_FREESTANDING_SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)
MACOS_FREESTANDING_TRIPLE ?= arm64-apple-macos11
TARGET_CC := $(MACOS_FREESTANDING_CC)
TARGET_ARCH_DIR := $(ROOT)/src/arch/$(TARGET_ARCH)/macos
COMMON_CFLAGS ?= -target $(MACOS_FREESTANDING_TRIPLE) $(if $(MACOS_FREESTANDING_SDKROOT),-isysroot $(MACOS_FREESTANDING_SDKROOT)) -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -fno-common -fno-jump-tables -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions -DNEWOS_MACOS_NEWLINKER -DNEWOS_CRYPTO_CURVE25519_FORCE_64BIT=1 -DNEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES=1 -DNEWOS_TOOL_DEFAULT_COLOR_NEVER=1 -flto -I$(ROOT)/src/shared -I$(ROOT)/src/compiler -I$(ROOT)/src/platform/macos -I$(TARGET_ARCH_DIR)
LDFLAGS ?= --macho-compact --gc-sections
TARGET_CC_TARGET_FLAG ?=
TARGET_BUILTINS_LIB ?=
PROFILE_RUNTIME_SOURCE := $(if $(filter 1 yes true,$(PROFILE)),$(ROOT)/src/platform/macos/profiler_runtime.c)
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
NO_ADDRSIG_CFLAGS ?= $(shell printf 'int x;\n' | "$(TARGET_CC)" $(TARGET_CC_TARGET_FLAG) -fno-addrsig -x c - -c -o "$(OBJ_DIR)/newos-gitd-addrsig-check.o" >/dev/null 2>&1 && echo -fno-addrsig; rm -f "$(OBJ_DIR)/newos-gitd-addrsig-check.o")
SIZE_CFLAGS ?= $(shell printf 'int x;\n' | "$(TARGET_CC)" $(TARGET_CC_TARGET_FLAG) -fcf-protection=none -falign-functions=1 -falign-jumps=1 -falign-loops=1 -falign-labels=1 -fomit-frame-pointer -fno-ident -x c - -c -o "$(OBJ_DIR)/newos-gitd-size-check.o" >/dev/null 2>&1 && echo -fcf-protection=none -falign-functions=1 -falign-jumps=1 -falign-loops=1 -falign-labels=1 -fomit-frame-pointer -fno-ident; rm -f "$(OBJ_DIR)/newos-gitd-size-check.o")
ifeq ($(FREESTANDING_USE_NEWLINKER),1)
COMMON_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fno-pic -fno-pie $(NO_ADDRSIG_CFLAGS) -fmerge-all-constants $(SIZE_CFLAGS) -DNEWOS_HAVE_PTHREAD=0 -DNEWOS_RUNTIME_THREAD_SAFE_ALLOC=1 -DNEWOS_RUNTIME_ALLOC_LOCK=1 -DNEWOS_DISABLE_STACK_GUARD_INIT=1 -I$(ROOT)/src/shared -I$(ROOT)/src/compiler -I$(ROOT)/src/platform/posix -I$(ROOT)/src/platform/linux -I$(ROOT)/src/platform/common -I$(TARGET_ARCH_DIR)
else
COMMON_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fPIE $(SIZE_CFLAGS) -DNEWOS_DISABLE_STACK_GUARD_INIT=1 -I$(ROOT)/src/shared -I$(ROOT)/src/compiler -I$(ROOT)/src/platform/posix -I$(ROOT)/src/platform/linux -I$(ROOT)/src/platform/common -I$(TARGET_ARCH_DIR)
endif
LDFLAGS ?= -nostdlib -static-pie -Wl,--gc-sections $(STRIP_LDFLAGS)
PROFILE_RUNTIME_SOURCE := $(if $(filter 1 yes true,$(PROFILE)),$(ROOT)/src/platform/linux/profiler_runtime.c)
endif

COMMON_CFLAGS += $(PROFILE_CFLAGS)

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

FREESTANDING_C_SOURCES := $(TLS_SOURCES) $(CRYPTO_SOURCES) $(TARGET_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) $(TARGET_PLATFORM_C_SOURCES) $(PROFILE_RUNTIME_SOURCE)
FREESTANDING_ASM_SOURCES := $(TARGET_PLATFORM_ASM_SOURCES) $(TARGET_CRT)
GITD_PRIVATE_SOURCES := $(wildcard gitd/*.c)
GITD_PRIVATE_HEADERS := $(wildcard gitd/*.h)
source_object = $(OBJ_DIR)/$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(patsubst $(ROOT)/%,%,$(1))))
FREESTANDING_OBJECTS := $(foreach src,$(FREESTANDING_C_SOURCES) $(FREESTANDING_ASM_SOURCES),$(call source_object,$(src)))
TARGET_CRT_OBJECT := $(call source_object,$(TARGET_CRT))
MACOS_CRT_OBJECT := $(call source_object,$(TARGET_CRT))

.PHONY: all freestanding profile benchmark-pack-quality test clean
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

profile: clean
	+$(MAKE) --no-print-directory PROFILE=1 LINKER_REPORTS=1 freestanding

benchmark-pack-quality: freestanding
	sh ./bench-pack-quality.sh

$(BUILD_DIR) $(OBJ_DIR) $(MAP_DIR):
	mkdir -p $@

ifeq ($(LOCAL_MACOS_FREESTANDING),1)
$(BUILD_DIR)/gitd: $(OBJ_DIR)/gitd.o $(FREESTANDING_OBJECTS) $(ROOT_LINKER) | $(BUILD_DIR) $(if $(filter 1 yes true,$(LINKER_REPORTS)),$(MAP_DIR))
	$(ROOT_LINKER) --target=mach-o-arm64 $(LDFLAGS) $(LINKER_REPORT_FLAGS) --lto-cc="$(TARGET_CC)" -o $@ $(MACOS_CRT_OBJECT) $< $(filter-out $(MACOS_CRT_OBJECT),$(FREESTANDING_OBJECTS))

$(ROOT_LINKER):
	$(MAKE) -C $(ROOT) $(ROOT_LINKER_TARGET)
else ifeq ($(FREESTANDING_USE_NEWLINKER),1)
$(BUILD_DIR)/gitd: $(OBJ_DIR)/gitd.o $(FREESTANDING_OBJECTS) $(ROOT_LINKER) | $(BUILD_DIR) $(if $(filter 1 yes true,$(LINKER_REPORTS)),$(MAP_DIR))
	$(ROOT_LINKER) $(NEWLINKER_FLAGS) $(LINKER_REPORT_FLAGS) -m x86_64-linux -o $@ $(TARGET_CRT_OBJECT) $< $(filter-out $(TARGET_CRT_OBJECT),$(FREESTANDING_OBJECTS))

$(ROOT_LINKER):
	$(MAKE) -C $(ROOT) $(ROOT_LINKER_TARGET)
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