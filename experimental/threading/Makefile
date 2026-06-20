CC ?= cc
TARGET_CC ?= $(CC)
ROOT := ../..
HOST_OS := $(shell uname -s 2>/dev/null || echo unknown)
HOST_ARCH_RAW := $(shell uname -m 2>/dev/null || echo unknown)
HOST_ARCH := $(if $(filter arm64,$(HOST_ARCH_RAW)),aarch64,$(HOST_ARCH_RAW))
LOCAL_MACOS_FREESTANDING := $(if $(filter Darwin,$(HOST_OS)),$(if $(filter aarch64,$(HOST_ARCH)),1,0),0)
TARGET_ARCH ?= $(if $(filter 1,$(LOCAL_MACOS_FREESTANDING)),aarch64,x86_64)
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/.obj/$(if $(filter 1,$(LOCAL_MACOS_FREESTANDING)),macos-$(TARGET_ARCH),linux-$(TARGET_ARCH))
PARALLEL_JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
PARALLEL_MAKEFLAGS := $(filter -j,$(MAKEFLAGS)) $(filter -j%,$(MAKEFLAGS)) $(filter --jobserver%,$(MAKEFLAGS))
PROFILE ?= 0
PROFILE_CFLAGS := $(if $(filter 1 yes true,$(PROFILE)),-finstrument-functions -fno-omit-frame-pointer -fno-inline -g)
STRIP_LDFLAGS := $(if $(filter 1 yes true,$(PROFILE)),,-Wl,-s)

ifeq ($(LOCAL_MACOS_FREESTANDING),1)
MACOS_FREESTANDING_CC ?= $(shell xcrun --find clang 2>/dev/null || echo $(CC))
MACOS_FREESTANDING_SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)
MACOS_FREESTANDING_TRIPLE ?= arm64-apple-macos11
ROOT_LINKER ?= $(ROOT)/build/linker
TARGET_CC := $(MACOS_FREESTANDING_CC)
TARGET_ARCH_DIR := $(ROOT)/src/arch/$(TARGET_ARCH)/macos
COMMON_CFLAGS ?= -target $(MACOS_FREESTANDING_TRIPLE) $(if $(MACOS_FREESTANDING_SDKROOT),-isysroot $(MACOS_FREESTANDING_SDKROOT)) -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -fno-common -fno-jump-tables -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions -DNEWOS_MACOS_NEWLINKER -DNEWOS_RUNTIME_THREAD_SAFE_ALLOC=1 -DNEWOS_RUNTIME_ALLOC_LOCK=3 -DNEWOS_TOOL_DEFAULT_COLOR_NEVER=1 -flto -I$(ROOT)/src/shared -I$(ROOT)/src/platform/macos -I$(TARGET_ARCH_DIR)
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
TARGET_CC_TARGET_FLAG ?= $(shell mkdir -p "$(OBJ_DIR)" && printf 'int main(void){return 0;}\n' | "$(TARGET_CC)" --target=$(TARGET_TRIPLE) -x c - -c -o "$(OBJ_DIR)/newos-threading-target-check.o" >/dev/null 2>&1 && echo --target=$(TARGET_TRIPLE); rm -f "$(OBJ_DIR)/newos-threading-target-check.o")
TARGET_BUILTINS_LIB ?= $(shell "$(TARGET_CC)" -print-libgcc-file-name >/dev/null 2>&1 && echo -lgcc || true)
COMMON_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fPIE -DNEWOS_DISABLE_STACK_GUARD_INIT=1 -DNEWOS_RUNTIME_THREAD_SAFE_ALLOC=1 -I$(ROOT)/src/shared -I$(ROOT)/src/platform/linux -I$(ROOT)/src/platform/common -I$(TARGET_ARCH_DIR)
LDFLAGS ?= -nostdlib -static-pie -Wl,--gc-sections $(STRIP_LDFLAGS)
TARGET_PLATFORM_C_SOURCES := $(ROOT)/src/platform/linux/io.c $(ROOT)/src/platform/linux/time.c $(ROOT)/src/platform/linux/thread.c
PROFILE_RUNTIME_SOURCE := $(if $(filter 1 yes true,$(PROFILE)),$(ROOT)/src/platform/linux/profiler_runtime.c)
TARGET_PLATFORM_ASM_SOURCES := $(wildcard $(TARGET_ARCH_DIR)/syscall_stubs.S)
TARGET_CRT := $(TARGET_ARCH_DIR)/crt0.S
endif

COMMON_CFLAGS += $(PROFILE_CFLAGS)

RUNTIME_SOURCES := \
    $(ROOT)/src/shared/runtime/memory.c \
    $(ROOT)/src/shared/runtime/string.c \
    $(ROOT)/src/shared/runtime/parse.c \
    $(ROOT)/src/shared/runtime/io.c \
    $(ROOT)/src/shared/runtime/concurrency.c

FREESTANDING_C_SOURCES := $(RUNTIME_SOURCES) $(TARGET_PLATFORM_C_SOURCES) $(PROFILE_RUNTIME_SOURCE)
FREESTANDING_ASM_SOURCES := $(TARGET_PLATFORM_ASM_SOURCES) $(TARGET_CRT)
source_object = $(OBJ_DIR)/$(patsubst %.S,%.o,$(patsubst %.c,%.o,$(patsubst $(ROOT)/%,%,$(1))))
FREESTANDING_OBJECTS := $(foreach src,$(FREESTANDING_C_SOURCES) $(FREESTANDING_ASM_SOURCES),$(call source_object,$(src)))
CRT_OBJECT := $(call source_object,$(TARGET_CRT))

.PHONY: all freestanding run report guide stress host-tools-bench profile profile-report test clean
.SECONDARY:

ifeq ($(AUTO_PARALLEL),1)
all: freestanding
else ifneq ($(strip $(PARALLEL_MAKEFLAGS)),)
all: freestanding
else
all:
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) freestanding
endif

freestanding: $(BUILD_DIR)/threadbench $(BUILD_DIR)/threadstress $(BUILD_DIR)/threadcompress $(BUILD_DIR)/threadread

run: freestanding
	$(BUILD_DIR)/threadbench

report: freestanding
	sh ./report.sh --no-build

guide: freestanding
	@tmp="$${TMPDIR:-/tmp}/threading-guide-$$$$.txt"; \
	{ printf '# section=width-sweep-cpu\n'; $(BUILD_DIR)/threadbench --case mix --items 65536 --rounds 8 --repeat 2 --max-width 4 --min-chunk 1; } > "$$tmp"; \
	sh ./benchguide.sh "$$tmp"; \
	rm -f "$$tmp"

stress: freestanding
	$(BUILD_DIR)/threadstress

compress-bench: freestanding
	$(BUILD_DIR)/threadcompress --case mixed --bytes $${THREADCOMPRESS_BYTES:-33554432} --chunk $${THREADCOMPRESS_CHUNK:-1048576} --repeat $${THREADCOMPRESS_REPEAT:-3} --max-width $${THREADBENCH_MAX_WIDTH:-24} --stats

host-tools-bench:
	python3 ./host-tool-bench.py --build

profile: clean
	+$(MAKE) --no-print-directory PROFILE=1 freestanding

profile-report: profile
	@mkdir -p $(ROOT)/tests/tmp
	@nm -n $(BUILD_DIR)/threadbench > $(ROOT)/tests/tmp/threadbench-profile.symbols
	@NEWOS_PROFILE=$(ROOT)/tests/tmp/threadbench-profile.nprof $(BUILD_DIR)/threadbench --case tasks --tasks 65536 --rounds 16 --repeat 1 --max-width 1 --stats > $(ROOT)/tests/tmp/threadbench-profile-run.txt
	@$(ROOT)/build/freestanding-linux-x86_64/profiler -m $(ROOT)/tests/tmp/threadbench-profile.symbols --sort self -n 25 $(ROOT)/tests/tmp/threadbench-profile.nprof > $(ROOT)/tests/tmp/threadbench-profile-summary.txt
	@printf '%s\n' 'wrote tests/tmp/threadbench-profile-run.txt and tests/tmp/threadbench-profile-summary.txt'

test: freestanding
	$(BUILD_DIR)/threadbench --case overhead --items 4096 --repeat 1 --max-width 2 >/dev/null
	$(BUILD_DIR)/threadbench --case mix --items 8192 --rounds 2 --repeat 1 --max-width 2 --min-chunk 1 --stats >/dev/null
	$(BUILD_DIR)/threadcompress --case mixed --bytes 1048576 --chunk 262144 --repeat 1 --max-width 2 --stats >/dev/null
	$(BUILD_DIR)/threadread --workers 2 threadbench.c threadstress.c threadcompress.c >/dev/null
	$(BUILD_DIR)/threadstress --iterations 8 --max-width 2 --items 4096 --rounds 2 --quiet >/dev/null
	@tmp="$${TMPDIR:-/tmp}/threading-guide-$$$$.txt"; \
	{ printf '# section=width-sweep-cpu\n'; $(BUILD_DIR)/threadbench --case mix --items 8192 --rounds 2 --repeat 1 --max-width 2 --min-chunk 1; } > "$$tmp"; \
	sh ./benchguide.sh "$$tmp" >/dev/null; \
	sh ./benchcmp.sh "$$tmp" "$$tmp" >/dev/null; \
	rm -f "$$tmp"

$(BUILD_DIR) $(OBJ_DIR):
	mkdir -p $@

ifeq ($(LOCAL_MACOS_FREESTANDING),1)
$(BUILD_DIR)/threadbench: $(OBJ_DIR)/threadbench.o $(FREESTANDING_OBJECTS) $(ROOT_LINKER) | $(BUILD_DIR)
	$(ROOT_LINKER) --target=mach-o-arm64 $(LDFLAGS) --lto-cc="$(TARGET_CC)" -o $@ $(CRT_OBJECT) $< $(filter-out $(CRT_OBJECT),$(FREESTANDING_OBJECTS))

$(BUILD_DIR)/threadstress: $(OBJ_DIR)/threadstress.o $(FREESTANDING_OBJECTS) $(ROOT_LINKER) | $(BUILD_DIR)
	$(ROOT_LINKER) --target=mach-o-arm64 $(LDFLAGS) --lto-cc="$(TARGET_CC)" -o $@ $(CRT_OBJECT) $< $(filter-out $(CRT_OBJECT),$(FREESTANDING_OBJECTS))

$(BUILD_DIR)/threadcompress: $(OBJ_DIR)/threadcompress.o $(FREESTANDING_OBJECTS) $(ROOT_LINKER) | $(BUILD_DIR)
	$(ROOT_LINKER) --target=mach-o-arm64 $(LDFLAGS) --lto-cc="$(TARGET_CC)" -o $@ $(CRT_OBJECT) $< $(filter-out $(CRT_OBJECT),$(FREESTANDING_OBJECTS))

$(BUILD_DIR)/threadread: $(OBJ_DIR)/threadread.o $(FREESTANDING_OBJECTS) $(ROOT_LINKER) | $(BUILD_DIR)
	$(ROOT_LINKER) --target=mach-o-arm64 $(LDFLAGS) --lto-cc="$(TARGET_CC)" -o $@ $(CRT_OBJECT) $< $(filter-out $(CRT_OBJECT),$(FREESTANDING_OBJECTS))

$(ROOT_LINKER):
	$(MAKE) -C $(ROOT) host
else
$(BUILD_DIR)/threadbench: $(OBJ_DIR)/threadbench.o $(FREESTANDING_OBJECTS) | $(BUILD_DIR)
	$(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) $< $(FREESTANDING_OBJECTS) $(LDFLAGS) $(TARGET_BUILTINS_LIB) -o $@

$(BUILD_DIR)/threadstress: $(OBJ_DIR)/threadstress.o $(FREESTANDING_OBJECTS) | $(BUILD_DIR)
	$(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) $< $(FREESTANDING_OBJECTS) $(LDFLAGS) $(TARGET_BUILTINS_LIB) -o $@

$(BUILD_DIR)/threadcompress: $(OBJ_DIR)/threadcompress.o $(FREESTANDING_OBJECTS) | $(BUILD_DIR)
	$(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) $< $(FREESTANDING_OBJECTS) $(LDFLAGS) $(TARGET_BUILTINS_LIB) -o $@

$(BUILD_DIR)/threadread: $(OBJ_DIR)/threadread.o $(FREESTANDING_OBJECTS) | $(BUILD_DIR)
	$(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) $< $(FREESTANDING_OBJECTS) $(LDFLAGS) $(TARGET_BUILTINS_LIB) -o $@
endif

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(ROOT)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(ROOT)/%.S | $(OBJ_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(COMMON_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)