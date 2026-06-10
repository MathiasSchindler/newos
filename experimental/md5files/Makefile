CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
ifneq ($(findstring ncc,$(CC)),)
  SHARED_INC_FLAG := -I../../src/shared
else
  SHARED_INC_FLAG := -idirafter ../../src/shared
endif

BUILD_DIR := build
OUT_DIR := out
MD5_SRC := ../../src/shared/crypto/md5.c
RUNTIME_SRCS := \
	../../src/shared/runtime/io.c \
	../../src/shared/runtime/memory.c \
	../../src/shared/runtime/parse.c \
	../../src/shared/runtime/string.c \
	../../src/shared/runtime/unicode.c \
	../../src/shared/runtime/unicode_utf8.c
PLATFORM_SRCS := \
	../../src/platform/posix/fs.c \
	../../src/platform/posix/identity.c \
	../../src/platform/posix/time.c \
	../../src/platform/posix/process.c
GENERATOR_SRCS := generate.c $(MD5_SRC) $(RUNTIME_SRCS) $(PLATFORM_SRCS)

.PHONY: all files verify elf-demo elf-hashclash-demo scaffold-examples clean

all: verify

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(BUILD_DIR)/generate: $(GENERATOR_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SHARED_INC_FLAG) -I../../src/platform/posix -I../../src/platform/common $(GENERATOR_SRCS) -o $(BUILD_DIR)/generate

files: $(BUILD_DIR)/generate | $(OUT_DIR)
	$(BUILD_DIR)/generate $(OUT_DIR)

verify: files
	sh ./verify.sh $(OUT_DIR)/md5file-a.bin $(OUT_DIR)/md5file-b.bin

elf-demo: $(BUILD_DIR)/generate | $(OUT_DIR)
	$(BUILD_DIR)/generate --elf-demo --out-dir $(OUT_DIR)

elf-hashclash-demo: elf-demo

scaffold-examples: $(BUILD_DIR)/generate | $(OUT_DIR)
	@set +e; \
	if [ -n "$(BACKEND)" ]; then \
		backend_command='$(value BACKEND)'; \
		$(BUILD_DIR)/generate --in1 examples/true --in2 examples/false --out-dir $(OUT_DIR) --backend "$$backend_command"; \
	else \
		$(BUILD_DIR)/generate --in1 examples/true --in2 examples/false --out-dir $(OUT_DIR); \
	fi; \
	status=$$?; \
	if [ $$status -eq 0 ]; then \
		echo "scaffold-examples: true/false collision produced"; \
	elif [ $$status -eq 2 ]; then \
		echo "scaffold-examples: true/false needs a chosen-prefix backend"; \
	else \
		exit $$status; \
	fi

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
