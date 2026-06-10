CC ?= cc
NVCC ?= nvcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
ifneq ($(findstring ncc,$(CC)),)
  SHARED_INC_FLAG := -I../../src/shared
else
  SHARED_INC_FLAG := -idirafter ../../src/shared
endif

BUILD_DIR := build
OUT_DIR := out
HASH_SRCS := \
	../../src/shared/crypto/md5.c \
	../../src/shared/crypto/sha1.c
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
GENERATOR_SRCS := generate.c $(HASH_SRCS) $(RUNTIME_SRCS) $(PLATFORM_SRCS)
TOY_SHA1_BACKEND_SRCS := toy_sha1_backend.c ../../src/shared/crypto/sha1.c
TOY_SHA1_CUDA_BACKEND_SRCS := toy_sha1_cuda_backend.cu

.PHONY: all files sha1-files verify verify-sha1 elf-demo elf-hashclash-demo scaffold-examples toy-sha1-elf-demo toy-sha1-32-elf-demo toy-sha1-40-elf-demo toy-sha1-48-elf-demo toy-sha1-56-elf-attempt toy-sha1-64-elf-attempt toy-sha1-scale toy-sha1-large-scale toy-sha1-48-cuda-demo toy-sha1-56-cuda-attempt toy-sha1-64-cuda-attempt toy-sha1-64-cuda-batched toy-sha1-cuda-large-scale clean

all: verify

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(BUILD_DIR)/generate: $(GENERATOR_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SHARED_INC_FLAG) -I../../src/platform/posix -I../../src/platform/common $(GENERATOR_SRCS) -o $(BUILD_DIR)/generate

$(BUILD_DIR)/toy_sha1_backend: $(TOY_SHA1_BACKEND_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SHARED_INC_FLAG) $(TOY_SHA1_BACKEND_SRCS) -o $(BUILD_DIR)/toy_sha1_backend

$(BUILD_DIR)/toy_sha1_cuda_backend: $(TOY_SHA1_CUDA_BACKEND_SRCS) | $(BUILD_DIR)
	$(NVCC) -O3 $(TOY_SHA1_CUDA_BACKEND_SRCS) -o $(BUILD_DIR)/toy_sha1_cuda_backend

files: $(BUILD_DIR)/generate | $(OUT_DIR)
	$(BUILD_DIR)/generate $(OUT_DIR)

sha1-files: $(BUILD_DIR)/generate | $(OUT_DIR)
	$(BUILD_DIR)/generate --hash sha1 $(OUT_DIR)

verify: files
	sh ./verify.sh $(OUT_DIR)/md5file-a.bin $(OUT_DIR)/md5file-b.bin

verify-sha1: sha1-files
	HASH=sha1 sh ./verify.sh $(OUT_DIR)/sha1file-a.bin $(OUT_DIR)/sha1file-b.bin

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

toy-sha1-elf-demo: $(BUILD_DIR)/generate $(BUILD_DIR)/toy_sha1_backend | $(OUT_DIR)
	$(BUILD_DIR)/generate --hash sha1-24 --in1 examples/true --in2 examples/false --out-dir $(OUT_DIR) --backend $(BUILD_DIR)/toy_sha1_backend
	$(OUT_DIR)/elf-sha1-24file-a.bin
	@set +e; $(OUT_DIR)/elf-sha1-24file-b.bin; status=$$?; set -e; test $$status -eq 1

toy-sha1-32-elf-demo: $(BUILD_DIR)/generate $(BUILD_DIR)/toy_sha1_backend | $(OUT_DIR)
	$(BUILD_DIR)/generate --hash sha1-32 --in1 examples/true --in2 examples/false --out-dir $(OUT_DIR) --backend $(BUILD_DIR)/toy_sha1_backend
	$(OUT_DIR)/elf-sha1-32file-a.bin
	@set +e; $(OUT_DIR)/elf-sha1-32file-b.bin; status=$$?; set -e; test $$status -eq 1

toy-sha1-40-elf-demo: $(BUILD_DIR)/generate $(BUILD_DIR)/toy_sha1_backend | $(OUT_DIR)
	$(BUILD_DIR)/generate --hash sha1-40 --in1 examples/true --in2 examples/false --out-dir $(OUT_DIR) --backend $(BUILD_DIR)/toy_sha1_backend
	$(OUT_DIR)/elf-sha1-40file-a.bin
	@set +e; $(OUT_DIR)/elf-sha1-40file-b.bin; status=$$?; set -e; test $$status -eq 1

toy-sha1-48-elf-demo: $(BUILD_DIR)/generate $(BUILD_DIR)/toy_sha1_backend | $(OUT_DIR)
	$(BUILD_DIR)/generate --hash sha1-48 --in1 examples/true --in2 examples/false --out-dir $(OUT_DIR) --backend $(BUILD_DIR)/toy_sha1_backend
	$(OUT_DIR)/elf-sha1-48file-a.bin
	@set +e; $(OUT_DIR)/elf-sha1-48file-b.bin; status=$$?; set -e; test $$status -eq 1

toy-sha1-56-elf-attempt: $(BUILD_DIR)/generate $(BUILD_DIR)/toy_sha1_backend | $(OUT_DIR)
	@set +e; \
	$(BUILD_DIR)/generate --hash sha1-56 --in1 examples/true --in2 examples/false --out-dir $(OUT_DIR) --backend $(BUILD_DIR)/toy_sha1_backend; \
	status=$$?; set -e; \
	if [ $$status -eq 0 ]; then \
		$(OUT_DIR)/elf-sha1-56file-a.bin; \
		set +e; $(OUT_DIR)/elf-sha1-56file-b.bin; child=$$?; set -e; test $$child -eq 1; \
	else \
		echo "sha1-56: no collision found within bounded toy search"; \
	fi

toy-sha1-64-elf-attempt: $(BUILD_DIR)/generate $(BUILD_DIR)/toy_sha1_backend | $(OUT_DIR)
	@set +e; \
	$(BUILD_DIR)/generate --hash sha1-64 --in1 examples/true --in2 examples/false --out-dir $(OUT_DIR) --backend $(BUILD_DIR)/toy_sha1_backend; \
	status=$$?; set -e; \
	if [ $$status -eq 0 ]; then \
		$(OUT_DIR)/elf-sha1-64file-a.bin; \
		set +e; $(OUT_DIR)/elf-sha1-64file-b.bin; child=$$?; set -e; test $$child -eq 1; \
	else \
		echo "sha1-64: no collision found within bounded toy search"; \
	fi

toy-sha1-scale: $(BUILD_DIR)/generate $(BUILD_DIR)/toy_sha1_backend | $(OUT_DIR)
	/usr/bin/time -f 'sha1-24 wall=%e user=%U sys=%S maxrss=%MKB' $(MAKE) --no-print-directory toy-sha1-elf-demo
	/usr/bin/time -f 'sha1-32 wall=%e user=%U sys=%S maxrss=%MKB' $(MAKE) --no-print-directory toy-sha1-32-elf-demo
	/usr/bin/time -f 'sha1-40 wall=%e user=%U sys=%S maxrss=%MKB' $(MAKE) --no-print-directory toy-sha1-40-elf-demo

toy-sha1-large-scale: $(BUILD_DIR)/generate $(BUILD_DIR)/toy_sha1_backend | $(OUT_DIR)
	/usr/bin/time -f 'sha1-48 wall=%e user=%U sys=%S maxrss=%MKB' $(MAKE) --no-print-directory toy-sha1-48-elf-demo
	/usr/bin/time -f 'sha1-56 wall=%e user=%U sys=%S maxrss=%MKB' $(MAKE) --no-print-directory toy-sha1-56-elf-attempt
	/usr/bin/time -f 'sha1-64 wall=%e user=%U sys=%S maxrss=%MKB' $(MAKE) --no-print-directory toy-sha1-64-elf-attempt

toy-sha1-48-cuda-demo: $(BUILD_DIR)/generate $(BUILD_DIR)/toy_sha1_cuda_backend | $(OUT_DIR)
	$(BUILD_DIR)/generate --hash sha1-48 --in1 examples/true --in2 examples/false --out-dir $(OUT_DIR) --backend $(BUILD_DIR)/toy_sha1_cuda_backend
	$(OUT_DIR)/elf-sha1-48file-a.bin
	@set +e; $(OUT_DIR)/elf-sha1-48file-b.bin; status=$$?; set -e; test $$status -eq 1

toy-sha1-56-cuda-attempt: $(BUILD_DIR)/generate $(BUILD_DIR)/toy_sha1_cuda_backend | $(OUT_DIR)
	@set +e; \
	$(BUILD_DIR)/generate --hash sha1-56 --in1 examples/true --in2 examples/false --out-dir $(OUT_DIR) --backend $(BUILD_DIR)/toy_sha1_cuda_backend; \
	status=$$?; set -e; \
	if [ $$status -eq 0 ]; then \
		$(OUT_DIR)/elf-sha1-56file-a.bin; \
		set +e; $(OUT_DIR)/elf-sha1-56file-b.bin; child=$$?; set -e; test $$child -eq 1; \
	else \
		echo "sha1-56 CUDA: no collision found within bounded toy search"; \
	fi

toy-sha1-64-cuda-attempt: $(BUILD_DIR)/generate $(BUILD_DIR)/toy_sha1_cuda_backend | $(OUT_DIR)
	@set +e; \
	$(BUILD_DIR)/generate --hash sha1-64 --in1 examples/true --in2 examples/false --out-dir $(OUT_DIR) --backend $(BUILD_DIR)/toy_sha1_cuda_backend; \
	status=$$?; set -e; \
	if [ $$status -eq 0 ]; then \
		$(OUT_DIR)/elf-sha1-64file-a.bin; \
		set +e; $(OUT_DIR)/elf-sha1-64file-b.bin; child=$$?; set -e; test $$child -eq 1; \
	else \
		echo "sha1-64 CUDA: no collision found within bounded toy search"; \
	fi

toy-sha1-64-cuda-batched: $(BUILD_DIR)/generate $(BUILD_DIR)/toy_sha1_cuda_backend | $(OUT_DIR)
	@set +e; \
	NEWOS_TOY_A_CANDIDATES=268435456 \
	NEWOS_TOY_B_LIMIT=268435456 \
	NEWOS_TOY_TABLE_CAPACITY=1073741824 \
	NEWOS_TOY_BATCHES=$${NEWOS_TOY_BATCHES:-256} \
	$(BUILD_DIR)/generate --hash sha1-64 --in1 examples/true --in2 examples/false --out-dir $(OUT_DIR) --backend $(BUILD_DIR)/toy_sha1_cuda_backend; \
	status=$$?; set -e; \
	if [ $$status -eq 0 ]; then \
		$(OUT_DIR)/elf-sha1-64file-a.bin; \
		set +e; $(OUT_DIR)/elf-sha1-64file-b.bin; child=$$?; set -e; test $$child -eq 1; \
	else \
		echo "sha1-64 CUDA batched: no collision found within batched toy search"; \
	fi

toy-sha1-cuda-large-scale: $(BUILD_DIR)/generate $(BUILD_DIR)/toy_sha1_cuda_backend | $(OUT_DIR)
	/usr/bin/time -f 'sha1-48-cuda wall=%e user=%U sys=%S maxrss=%MKB' $(MAKE) --no-print-directory toy-sha1-48-cuda-demo
	/usr/bin/time -f 'sha1-56-cuda wall=%e user=%U sys=%S maxrss=%MKB' $(MAKE) --no-print-directory toy-sha1-56-cuda-attempt
	/usr/bin/time -f 'sha1-64-cuda wall=%e user=%U sys=%S maxrss=%MKB' $(MAKE) --no-print-directory toy-sha1-64-cuda-attempt

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
