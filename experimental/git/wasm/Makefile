ROOT := ../../..
BUILD_DIR := build
WASM_CC := $(shell if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then echo /opt/homebrew/opt/llvm/bin/clang; elif command -v clang >/dev/null 2>&1; then command -v clang; else echo clang; fi)
ifeq ($(origin CC),default)
CC := $(WASM_CC)
endif

WASM_CFLAGS := \
	--target=wasm32-unknown-unknown \
	-std=c11 \
	-Wall -Wextra -Wpedantic \
	-Oz \
	-ffreestanding -fno-builtin -fno-common \
	-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections \
	-DNEWOS_WASM=1 \
	-DRT_LARGE_CACHE_MAX_BYTES=0 \
	-I$(ROOT)/src/shared \
	-I$(ROOT)/src/compiler

WASM_LDFLAGS := \
	-nostdlib \
	-Wl,--no-entry \
	-Wl,--export-memory \
	-Wl,--gc-sections \
	-Wl,-z,stack-size=8388608 \
	-Wl,--export=newos_wasm_alloc \
	-Wl,--export=newos_wasm_free \
	-Wl,--export=newos_wasm_reset \
	-Wl,--export=newos_wasm_write_file \
	-Wl,--export=newos_wasm_read_file \
	-Wl,--export=newos_wasm_file_size \
	-Wl,--export=newos_wasm_list_files \
	-Wl,--export=newos_wasm_remove_path \
	-Wl,--export=newos_wasm_stdout_ptr \
	-Wl,--export=newos_wasm_stdout_size \
	-Wl,--export=newos_wasm_stderr_ptr \
	-Wl,--export=newos_wasm_stderr_size \
	-Wl,--export=newos_git_run_line \
	-Wl,--export=newos_git_clone_from_pack

RUNTIME_SOURCES := \
	$(ROOT)/src/shared/runtime/memory.c \
	$(ROOT)/src/shared/runtime/string.c \
	$(ROOT)/src/shared/runtime/parse.c \
	$(ROOT)/src/shared/runtime/io.c \
	$(ROOT)/src/shared/runtime/unicode_utf8.c \
	$(ROOT)/src/shared/runtime/unicode.c

TOOL_SOURCES := \
	$(ROOT)/src/shared/tool_json.c \
	$(ROOT)/src/shared/tool_cli.c \
	$(ROOT)/src/shared/tool_file.c \
	$(ROOT)/src/shared/tool_io.c \
	$(ROOT)/src/shared/tool_path.c \
	$(ROOT)/src/shared/tool_fs.c \
	$(ROOT)/src/shared/tool_regex.c \
	$(ROOT)/src/shared/tool_process.c

GIT_SUPPORT_SOURCES := \
	$(ROOT)/src/shared/crypto/sha1.c \
	$(ROOT)/src/shared/compression/crc32.c \
	$(ROOT)/src/shared/compression/zlib.c

EXPERIMENT_SOURCES := \
	platform_wasm.c \
	git_wasm.c

all: $(BUILD_DIR)/git.wasm

test: all
	node smoke.mjs

test-gitd: all
	node smoke-gitd.mjs

$(BUILD_DIR)/git.wasm: Makefile $(EXPERIMENT_SOURCES) $(RUNTIME_SOURCES) $(TOOL_SOURCES) $(GIT_SUPPORT_SOURCES) $(ROOT)/src/tools/git.c $(ROOT)/src/tools/git/*.c | $(BUILD_DIR)
	$(CC) $(WASM_CFLAGS) $(EXPERIMENT_SOURCES) $(RUNTIME_SOURCES) $(TOOL_SOURCES) $(GIT_SUPPORT_SOURCES) $(WASM_LDFLAGS) -o $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all test test-gitd clean
