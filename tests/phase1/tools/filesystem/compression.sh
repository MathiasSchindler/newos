#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/compression"
note "phase1 filesystem compression"

printf 'alpha\nbeta\n' > "$WORK_DIR/sample.txt"

assert_command_succeeds "${TEST_BIN_DIR}/gzip" -c "$WORK_DIR/sample.txt" > "$WORK_DIR/sample.txt.gz"
assert_command_succeeds "${TEST_BIN_DIR}/gunzip" -c "$WORK_DIR/sample.txt.gz" > "$WORK_DIR/gzip.out"
assert_files_equal "$WORK_DIR/sample.txt" "$WORK_DIR/gzip.out" "gzip/gunzip -c did not preserve file contents"

cp "$WORK_DIR/sample.txt" "$WORK_DIR/bzip-input.txt"
assert_command_succeeds "${TEST_BIN_DIR}/bzip2" "$WORK_DIR/bzip-input.txt"
[ -f "$WORK_DIR/bzip-input.txt.bz2" ] || fail "bzip2 did not create a .bz2 file"
assert_command_succeeds "${TEST_BIN_DIR}/bunzip2" "$WORK_DIR/bzip-input.txt.bz2"
assert_files_equal "$WORK_DIR/sample.txt" "$WORK_DIR/bzip-input.txt" "bunzip2 did not restore the original data"

if command -v python3 >/dev/null 2>&1 && python3 - <<'PY' >/dev/null 2>&1
import bz2
PY
then
	python3 - "$WORK_DIR/sample.txt" "$WORK_DIR/real-bzip.txt.bz2" <<'PY'
import bz2, pathlib, sys
pathlib.Path(sys.argv[2]).write_bytes(bz2.compress(pathlib.Path(sys.argv[1]).read_bytes()))
PY
	assert_command_succeeds "${TEST_BIN_DIR}/bunzip2" "$WORK_DIR/real-bzip.txt.bz2"
	assert_files_equal "$WORK_DIR/sample.txt" "$WORK_DIR/real-bzip.txt" "bunzip2 did not decode a standard bzip2 stream"
fi

cp "$WORK_DIR/sample.txt" "$WORK_DIR/xz-input.txt"
assert_command_succeeds "${TEST_BIN_DIR}/xz" "$WORK_DIR/xz-input.txt"
[ -f "$WORK_DIR/xz-input.txt.xz" ] || fail "xz did not create a .xz file"
assert_command_succeeds "${TEST_BIN_DIR}/unxz" "$WORK_DIR/xz-input.txt.xz"
assert_files_equal "$WORK_DIR/sample.txt" "$WORK_DIR/xz-input.txt" "unxz did not restore the original data"

if command -v zstd >/dev/null 2>&1 && command -v cc >/dev/null 2>&1; then
	cat > "$WORK_DIR/zstd_decode_smoke.c" <<'EOF'
#include "compression/zstd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *rt_malloc(size_t size) { return malloc(size == 0U ? 1U : size); }
void *rt_realloc(void *ptr, size_t size) { return realloc(ptr, size == 0U ? 1U : size); }
void rt_free(void *ptr) { free(ptr); }
void *rt_malloc_array(size_t count, size_t item_size) {
	if (item_size != 0U && count > (size_t)-1 / item_size) return NULL;
	return malloc(count * item_size == 0U ? 1U : count * item_size);
}
void *rt_realloc_array(void *ptr, size_t count, size_t item_size) {
	if (item_size != 0U && count > (size_t)-1 / item_size) return NULL;
	return realloc(ptr, count * item_size == 0U ? 1U : count * item_size);
}
void rt_memset(void *buffer, int byte_value, size_t count) { memset(buffer, byte_value, count); }

static unsigned char *read_file(const char *path, size_t *size_out) {
	FILE *file = fopen(path, "rb");
	unsigned char *buffer;
	long size;

	if (file == NULL) return NULL;
	if (fseek(file, 0, SEEK_END) != 0) return NULL;
	size = ftell(file);
	if (size < 0 || fseek(file, 0, SEEK_SET) != 0) return NULL;
	buffer = (unsigned char *)malloc((size_t)size == 0U ? 1U : (size_t)size);
	if (buffer == NULL) return NULL;
	if (fread(buffer, 1U, (size_t)size, file) != (size_t)size) return NULL;
	fclose(file);
	*size_out = (size_t)size;
	return buffer;
}

int main(int argc, char **argv) {
	size_t src_size = 0U;
	size_t dst_size = 0U;
	size_t written = 0U;
	unsigned char *src;
	unsigned char *dst;
	FILE *out;
	CompressionZstdResult result;

	if (argc != 3) return 2;
	src = read_file(argv[1], &src_size);
	if (src == NULL) return 3;
	result = compression_zstd_frame_content_size(src, src_size, &dst_size);
	if (result.status != COMPRESSION_ZSTD_OK) return 4;
	dst = (unsigned char *)malloc(dst_size == 0U ? 1U : dst_size);
	if (dst == NULL) return 5;
	result = compression_zstd_decompress_frame(dst, dst_size, src, src_size, &written);
	if (result.status != COMPRESSION_ZSTD_OK || written != dst_size) return 6;
	out = fopen(argv[2], "wb");
	if (out == NULL) return 7;
	if (fwrite(dst, 1U, written, out) != written) return 8;
	fclose(out);
	free(dst);
	free(src);
	return 0;
}
EOF
	cc -std=c11 -O2 -I"$ROOT_DIR/src/shared" "$WORK_DIR/zstd_decode_smoke.c" "$ROOT_DIR/src/shared/compression/zstd.c" -o "$WORK_DIR/zstd_decode_smoke"
	awk 'BEGIN { for (i = 0; i < 4096; ++i) print "zstd smoke line", i, "abcabcabcabcabc" }' > "$WORK_DIR/zstd-input.txt"
	zstd -q -f -3 "$WORK_DIR/zstd-input.txt" -o "$WORK_DIR/zstd-input.txt.zst"
	assert_command_succeeds "$WORK_DIR/zstd_decode_smoke" "$WORK_DIR/zstd-input.txt.zst" "$WORK_DIR/zstd.out"
	assert_files_equal "$WORK_DIR/zstd-input.txt" "$WORK_DIR/zstd.out" "shared zstd decoder did not restore the original data"
fi
