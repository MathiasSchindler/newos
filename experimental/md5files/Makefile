CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2

BUILD_DIR := build
OUT_DIR := out
MD5_SRC := ../../src/shared/crypto/md5.c

.PHONY: all files verify clean

all: verify

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(BUILD_DIR)/generate: generate.c $(MD5_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I../../src/shared generate.c $(MD5_SRC) -o $(BUILD_DIR)/generate

files: $(BUILD_DIR)/generate | $(OUT_DIR)
	$(BUILD_DIR)/generate $(OUT_DIR)

verify: files
	sh ./verify.sh $(OUT_DIR)/md5file-a.bin $(OUT_DIR)/md5file-b.bin

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)