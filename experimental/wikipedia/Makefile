CC ?= cc
TARGET_CC ?= $(CC)
TARGET_ARCH ?= x86_64
ROOT := ../..
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/.obj
TARGET_ARCH_DIR := $(ROOT)/src/arch/$(TARGET_ARCH)/linux
PARALLEL_JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
PARALLEL_MAKEFLAGS := $(filter -j,$(MAKEFLAGS)) $(filter -j%,$(MAKEFLAGS)) $(filter --jobserver%,$(MAKEFLAGS))

ifeq ($(TARGET_ARCH),x86_64)
TARGET_TRIPLE ?= x86_64-linux-none
else ifeq ($(TARGET_ARCH),aarch64)
TARGET_TRIPLE ?= aarch64-linux-none
else
$(error Unsupported TARGET_ARCH '$(TARGET_ARCH)'; expected x86_64 or aarch64)
endif

TARGET_CC_TARGET_FLAG ?= $(shell printf 'int main(void){return 0;}\n' | "$(TARGET_CC)" --target=$(TARGET_TRIPLE) -x c - -c -o /tmp/newos-wp-target-check.o >/dev/null 2>&1 && echo --target=$(TARGET_TRIPLE); rm -f /tmp/newos-wp-target-check.o)
TARGET_BUILTINS_LIB ?= $(shell "$(TARGET_CC)" -print-libgcc-file-name >/dev/null 2>&1 && echo -lgcc || true)

COMMON_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fPIE -DNEWOS_DISABLE_STACK_GUARD_INIT=1 -I$(ROOT)/src/shared -I$(ROOT)/src/compiler -I$(ROOT)/src/platform/posix -I$(ROOT)/src/platform/linux -I$(ROOT)/src/platform/common -I$(TARGET_ARCH_DIR)
LDFLAGS ?= -nostdlib -static-pie -Wl,--gc-sections -Wl,-s

SHARED_SOURCES := $(addprefix $(ROOT)/,$(shell cd $(ROOT) && grep -oE '"src/shared/(runtime/[^"]+|compression/[^"]+|tool_[^"]+|archive_util|object_util|archive_zip|bignum|simple_config|server_log|xml|xml_stream|xml_dtd)\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u))
TLS_SOURCES := $(addprefix $(ROOT)/,$(shell cd $(ROOT) && grep -oE '"src/shared/tls/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u))
CRYPTO_SOURCES := $(addprefix $(ROOT)/,$(shell cd $(ROOT) && grep -oE '"src/shared/crypto/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u))
TARGET_PLATFORM_MANIFEST_SOURCES := $(shell cd $(ROOT) && grep -oE '"src/platform/linux/[^"]+\.c"|"src/arch/[^"]+/linux/[^"]+\.(c|S)"' src/compiler/source_manifest.h | tr -d '"')
TARGET_PLATFORM_SOURCES := $(addprefix $(ROOT)/,$(filter src/platform/linux/% src/arch/$(TARGET_ARCH)/linux/%,$(TARGET_PLATFORM_MANIFEST_SOURCES)))
TARGET_PLATFORM_C_SOURCES := $(filter %.c,$(TARGET_PLATFORM_SOURCES))
TARGET_PLATFORM_ASM_SOURCES := $(filter %.S,$(TARGET_PLATFORM_SOURCES))
TARGET_TLS_PLATFORM_SOURCE := $(ROOT)/src/platform/linux/tls.c
TARGET_CRT := $(TARGET_ARCH_DIR)/crt0.S
TOOLS := wp-download wp-cite-extract
FREESTANDING_C_SOURCES := $(TLS_SOURCES) $(CRYPTO_SOURCES) $(TARGET_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) $(TARGET_PLATFORM_C_SOURCES)
FREESTANDING_ASM_SOURCES := $(TARGET_PLATFORM_ASM_SOURCES) $(TARGET_CRT)
source_object = $(OBJ_DIR)/$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(patsubst $(ROOT)/%,%,$(1))))
FREESTANDING_OBJECTS := $(foreach src,$(FREESTANDING_C_SOURCES) $(FREESTANDING_ASM_SOURCES),$(call source_object,$(src)))

.PHONY: all freestanding clean
.SECONDARY: $(addprefix $(OBJ_DIR)/,$(addsuffix .o,$(TOOLS)))

ifeq ($(AUTO_PARALLEL),1)
all: freestanding
else ifneq ($(strip $(PARALLEL_MAKEFLAGS)),)
all: freestanding
else
all:
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) freestanding
endif

freestanding: $(addprefix $(BUILD_DIR)/,$(TOOLS))

$(BUILD_DIR) $(OBJ_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%: $(OBJ_DIR)/%.o $(FREESTANDING_OBJECTS) | $(BUILD_DIR)
	$(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) $< $(FREESTANDING_OBJECTS) $(LDFLAGS) $(TARGET_BUILTINS_LIB) -o $@

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(ROOT)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(ROOT)/%.S | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)