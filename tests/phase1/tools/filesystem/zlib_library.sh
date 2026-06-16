#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/zlib_library"
note "phase1 filesystem zlib library"

cat > "$WORK_DIR/zlib_test.c" <<'EOF'
#include "compression/zlib.h"

#include <stdlib.h>
#include <string.h>

void rt_memset(void *buffer, int byte_value, size_t count) {
    (void)memset(buffer, byte_value, count);
}

void *rt_malloc(size_t size) {
    return malloc(size == 0 ? 1 : size);
}

void rt_free(void *ptr) {
    free(ptr);
}

static int bytes_equal(const unsigned char *left, const unsigned char *right, size_t length) {
    size_t index;

    for (index = 0; index < length; ++index) {
        if (left[index] != right[index]) return 0;
    }
    return 1;
}

static int roundtrip(int (*compress_fn)(const unsigned char *, size_t, unsigned char *, size_t, size_t *),
                     size_t (*bound_fn)(size_t)) {
    static const unsigned char input[] =
        "alpha alpha alpha alpha\n"
        "beta beta beta beta\n"
        "012345678901234567890123456789\n"
        "alpha alpha alpha alpha\n";
    unsigned char compressed[1024];
    unsigned char inflated[sizeof(input)];
    size_t compressed_size = 0;
    size_t inflated_size = 0;
    size_t bound = bound_fn(sizeof(input) - 1U);

    if (bound == 0 || bound > sizeof(compressed)) return 1;
    if (compress_fn(input, sizeof(input) - 1U, compressed, sizeof(compressed), &compressed_size) != 0) return 2;
    if (compressed_size == 0 || compressed_size > bound) return 3;
    if (compression_zlib_inflate(compressed, compressed_size, inflated, sizeof(inflated), &inflated_size) != 0) return 4;
    if (inflated_size != sizeof(input) - 1U) return 5;
    if (!bytes_equal(input, inflated, inflated_size)) return 6;
    return 0;
}

int main(void) {
    if (roundtrip(compression_zlib_store, compression_zlib_store_bound) != 0) return 1;
    if (roundtrip(compression_zlib_fixed_rle, compression_zlib_fixed_rle_bound) != 0) return 2;
    if (roundtrip(compression_zlib_fixed_lz77, compression_zlib_fixed_lz77_bound) != 0) return 3;
    {
        static const unsigned char input[] =
            "dynamic dynamic dynamic dynamic dynamic\n"
            "huffman huffman huffman huffman huffman\n"
            "lazy matching should still roundtrip correctly\n"
            "dynamic dynamic dynamic dynamic dynamic\n";
        unsigned char compressed[2048];
        unsigned char inflated[sizeof(input)];
        size_t compressed_size = 0;
        size_t inflated_size = 0;
        size_t bound = compression_zlib_deflate_bound(sizeof(input) - 1U);

        if (bound == 0 || bound > sizeof(compressed)) return 4;
        if (compression_zlib_deflate_level(input, sizeof(input) - 1U, compressed, sizeof(compressed), &compressed_size, 9) != 0) return 5;
        if (compressed_size == 0 || compressed_size > bound) return 6;
        if (compression_zlib_inflate(compressed, compressed_size, inflated, sizeof(inflated), &inflated_size) != 0) return 7;
        if (inflated_size != sizeof(input) - 1U) return 8;
        if (!bytes_equal(input, inflated, inflated_size)) return 9;
    }
    return 0;
}
EOF

cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/shared \
    "$WORK_DIR/zlib_test.c" src/shared/compression/zlib.c \
    -o "$WORK_DIR/zlib_test"
assert_command_succeeds "$WORK_DIR/zlib_test"