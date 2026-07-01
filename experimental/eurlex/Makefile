CC ?= cc
TARGET_CC ?= $(CC)
ROOT := ../..
HOST_OS := $(shell uname -s 2>/dev/null || echo unknown)
HOST_ARCH_RAW := $(shell uname -m 2>/dev/null || echo unknown)
HOST_ARCH := $(if $(filter arm64,$(HOST_ARCH_RAW)),aarch64,$(HOST_ARCH_RAW))
LOCAL_MACOS_FREESTANDING := $(if $(filter Darwin,$(HOST_OS)),$(if $(filter aarch64,$(HOST_ARCH)),1,0),0)
TARGET_ARCH ?= $(if $(filter 1,$(LOCAL_MACOS_FREESTANDING)),aarch64,x86_64)
BUILD_DIR := build
PROFILE ?= 0
PROFILE_CFLAGS := $(if $(filter 1 yes true,$(PROFILE)),-finstrument-functions -fno-omit-frame-pointer -fno-inline)
PROFILE_OBJ_SUFFIX := $(if $(filter 1 yes true,$(PROFILE)),-profile)
PROFILE_MAP := $(BUILD_DIR)/eurlex-delegated.map
PROFILE_LINKER_FLAGS := $(if $(filter 1 yes true,$(PROFILE)),--map $(PROFILE_MAP))
OBJ_DIR := $(BUILD_DIR)/.obj/$(if $(filter 1,$(LOCAL_MACOS_FREESTANDING)),macos-$(TARGET_ARCH),linux-$(TARGET_ARCH))$(PROFILE_OBJ_SUFFIX)
COMMA := ,

ifeq ($(LOCAL_MACOS_FREESTANDING),1)
MACOS_FREESTANDING_CC ?= $(shell xcrun --find clang 2>/dev/null || echo $(CC))
MACOS_FREESTANDING_SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)
MACOS_FREESTANDING_TRIPLE ?= arm64-apple-macos11
ROOT_LINKER ?= $(ROOT)/build/linker
TARGET_CC := $(MACOS_FREESTANDING_CC)
TARGET_ARCH_DIR := $(ROOT)/src/arch/$(TARGET_ARCH)/macos
COMMON_CFLAGS ?= -target $(MACOS_FREESTANDING_TRIPLE) $(if $(MACOS_FREESTANDING_SDKROOT),-isysroot $(MACOS_FREESTANDING_SDKROOT)) -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -fno-common -fno-jump-tables -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions -DNEWOS_MACOS_NEWLINKER -DNEWOS_RUNTIME_THREAD_SAFE_ALLOC=1 -DNEWOS_RUNTIME_ALLOC_LOCK=3 -DNEWOS_TOOL_DEFAULT_COLOR_NEVER=1 -flto $(PROFILE_CFLAGS) -I$(ROOT)/src/shared -I$(ROOT)/src/platform/macos -I$(TARGET_ARCH_DIR)
LDFLAGS ?= --macho-compact --gc-sections
TARGET_CC_TARGET_FLAG ?=
TARGET_BUILTINS_LIB ?=
TARGET_PLATFORM_C_SOURCES := $(ROOT)/src/platform/macos/freestanding.c $(ROOT)/src/platform/macos/newlinker_runtime.c
PROFILE_RUNTIME_SOURCE := $(if $(filter 1 yes true,$(PROFILE)),$(ROOT)/src/platform/macos/profiler_runtime.c)
TARGET_PLATFORM_ASM_SOURCES :=
TARGET_CRT := $(ROOT)/src/platform/macos/newlinker_start.S
else
TARGET_ARCH_DIR := $(ROOT)/src/arch/$(TARGET_ARCH)/linux
ifeq ($(TARGET_ARCH),x86_64)
TARGET_TRIPLE ?= x86_64-linux-none
else ifeq ($(TARGET_ARCH),aarch64)
TARGET_TRIPLE ?= aarch64-linux-none
else
$(error Unsupported TARGET_ARCH '$(TARGET_ARCH)'; expected x86_64 or aarch64)
endif
TARGET_CC_TARGET_FLAG ?= $(shell mkdir -p "$(OBJ_DIR)" && printf 'int main(void){return 0;}\n' | "$(TARGET_CC)" --target=$(TARGET_TRIPLE) -x c - -c -o "$(OBJ_DIR)/newos-eurlex-target-check.o" >/dev/null 2>&1 && echo --target=$(TARGET_TRIPLE); rm -f "$(OBJ_DIR)/newos-eurlex-target-check.o")
TARGET_BUILTINS_LIB ?= $(shell "$(TARGET_CC)" -print-libgcc-file-name >/dev/null 2>&1 && echo -lgcc || true)
COMMON_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fPIE -DNEWOS_DISABLE_STACK_GUARD_INIT=1 -DNEWOS_RUNTIME_THREAD_SAFE_ALLOC=1 $(PROFILE_CFLAGS) -I$(ROOT)/src/shared -I$(ROOT)/src/platform/linux -I$(ROOT)/src/platform/common -I$(TARGET_ARCH_DIR)
LDFLAGS ?= -nostdlib -static-pie -Wl,--gc-sections -Wl,-s
TARGET_PLATFORM_C_SOURCES := $(ROOT)/src/platform/linux/io.c $(ROOT)/src/platform/linux/time.c $(ROOT)/src/platform/linux/thread.c
PROFILE_RUNTIME_SOURCE := $(if $(filter 1 yes true,$(PROFILE)),$(ROOT)/src/platform/linux/profiler_runtime.c)
TARGET_PLATFORM_ASM_SOURCES := $(wildcard $(TARGET_ARCH_DIR)/syscall_stubs.S)
TARGET_CRT := $(TARGET_ARCH_DIR)/crt0.S
endif

HOST_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -ffunction-sections -fdata-sections
HOST_LDFLAGS ?= $(if $(filter Darwin,$(HOST_OS)),-Wl$(COMMA)-dead_strip,-Wl$(COMMA)--gc-sections)
ifneq ($(findstring ncc,$(CC)),)
  HOST_SHARED_INC_FLAG := -I$(ROOT)/src/shared
else
  HOST_SHARED_INC_FLAG := -idirafter $(ROOT)/src/shared
endif

TOOL_SOURCES := eurlex_delegated.c archive_util_min.c
SHARED_C_SOURCES := \
    $(ROOT)/src/shared/runtime/memory.c \
    $(ROOT)/src/shared/runtime/string.c \
    $(ROOT)/src/shared/runtime/parse.c \
    $(ROOT)/src/shared/runtime/io.c \
	$(ROOT)/src/shared/runtime/concurrency.c \
    $(ROOT)/src/shared/runtime/unicode_utf8.c \
    $(ROOT)/src/shared/runtime/unicode.c \
    $(ROOT)/src/shared/archive_zip.c \
    $(ROOT)/src/shared/compression/crc32.c \
	$(ROOT)/src/shared/compression/zlib.c \
	$(ROOT)/src/shared/xml.c
HOST_PLATFORM_C_SOURCES := \
    $(ROOT)/src/platform/posix/fs.c \
    $(ROOT)/src/platform/posix/process.c \
    $(ROOT)/src/platform/posix/identity.c \
	$(ROOT)/src/platform/posix/time.c \
	$(ROOT)/src/platform/posix/thread.c

FREESTANDING_C_SOURCES := $(TOOL_SOURCES) $(SHARED_C_SOURCES) $(TARGET_PLATFORM_C_SOURCES) $(PROFILE_RUNTIME_SOURCE)
FREESTANDING_ASM_SOURCES := $(TARGET_PLATFORM_ASM_SOURCES) $(TARGET_CRT)
source_object = $(OBJ_DIR)/$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(patsubst $(ROOT)/%,%,$(1))))
FREESTANDING_OBJECTS := $(foreach src,$(FREESTANDING_C_SOURCES) $(FREESTANDING_ASM_SOURCES),$(call source_object,$(src)))
CRT_OBJECT := $(call source_object,$(TARGET_CRT))
HOST_SOURCES := $(TOOL_SOURCES) $(SHARED_C_SOURCES) $(HOST_PLATFORM_C_SOURCES)

.PHONY: all freestanding host test test-host clean
.SECONDARY:

all: freestanding

freestanding: $(BUILD_DIR)/eurlex-delegated

host: $(BUILD_DIR)/host/eurlex-delegated

test: freestanding
	$(BUILD_DIR)/eurlex-delegated -a data/LEG_MTD_20260628_01_00.zip 32019R1021 > $(BUILD_DIR)/test-celex.tsv
	$(ROOT)/build/rg '32021R0115' $(BUILD_DIR)/test-celex.tsv
	$(BUILD_DIR)/eurlex-delegated -a data/LEG_MTD_20260628_01_00.zip '(EU) 2019/1021' > $(BUILD_DIR)/test-citation.tsv
	$(ROOT)/build/rg '32021R0115' $(BUILD_DIR)/test-citation.tsv

test-host: host
	$(BUILD_DIR)/host/eurlex-delegated -a data/LEG_MTD_20260628_01_00.zip 32019R1021 > $(BUILD_DIR)/host/test-celex.tsv
	$(ROOT)/build/rg '32021R0115' $(BUILD_DIR)/host/test-celex.tsv
	$(BUILD_DIR)/host/eurlex-delegated -a data/LEG_MTD_20260628_01_00.zip '(EU) 2019/1021' > $(BUILD_DIR)/host/test-citation.tsv
	$(ROOT)/build/rg '32021R0115' $(BUILD_DIR)/host/test-citation.tsv

$(BUILD_DIR) $(OBJ_DIR) $(BUILD_DIR)/host:
	mkdir -p $@

ifeq ($(LOCAL_MACOS_FREESTANDING),1)
$(BUILD_DIR)/eurlex-delegated: $(FREESTANDING_OBJECTS) $(ROOT_LINKER) | $(BUILD_DIR)
	$(ROOT_LINKER) --target=mach-o-arm64 $(LDFLAGS) $(PROFILE_LINKER_FLAGS) --lto-cc="$(TARGET_CC)" -o $@ $(CRT_OBJECT) $(filter-out $(CRT_OBJECT),$(FREESTANDING_OBJECTS))

$(ROOT_LINKER):
	$(MAKE) -C $(ROOT) host
else
$(BUILD_DIR)/eurlex-delegated: $(FREESTANDING_OBJECTS) | $(BUILD_DIR)
	$(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) $(CRT_OBJECT) $(filter-out $(CRT_OBJECT),$(FREESTANDING_OBJECTS)) $(LDFLAGS) $(TARGET_BUILTINS_LIB) -o $@
endif

$(BUILD_DIR)/host/eurlex-delegated: $(HOST_SOURCES) | $(BUILD_DIR)/host
	$(CC) $(HOST_CFLAGS) $(HOST_SHARED_INC_FLAG) -I$(ROOT)/src/platform/posix -I$(ROOT)/src/platform/common -I$(ROOT)/src/compiler $(HOST_SOURCES) $(HOST_LDFLAGS) -o $@

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(ROOT)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(ROOT)/%.S | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
