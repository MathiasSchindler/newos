#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "system/unicode_runtime"
note "phase1 system: Unicode runtime"

cat > "$WORK_DIR/unicode_runtime_test.c" <<'EOF'
#include "runtime.h"

#include <stdlib.h>
#include <string.h>

void *rt_realloc(void *pointer, size_t size) { return realloc(pointer, size); }
void rt_free(void *pointer) { free(pointer); }
void *rt_realloc_array(void *pointer, size_t count, size_t item_size) {
    if (item_size != 0U && count > ((size_t)-1) / item_size) return 0;
    return realloc(pointer, count * item_size);
}
void rt_memset(void *buffer, int value, size_t count) { (void)memset(buffer, value, count); }

static int bytes_equal(const void *left, size_t left_length, const void *right, size_t right_length) {
    return left_length == right_length && memcmp(left, right, left_length) == 0;
}

int main(void) {
    static const char composed[] = "caf\303\251";
    static const char decomposed[] = "cafe\314\201";
    static const char marks_a[] = "a\314\243\314\201";
    static const char marks_b[] = "a\314\201\314\243";
    static const char hangul[] = "\352\260\200";
    static const char jamo[] = "\341\204\200\341\205\241";
    static const unsigned char utf16le[] = {
        0xffU, 0xfeU, 'c', 0U, 'a', 0U, 'f', 0U, 0xe9U, 0U, 0x3dU, 0xd8U, 0x42U, 0xdeU
    };
    static const char utf8_text[] = "caf\303\251\360\237\231\202";
    static const unsigned char utf16be_expected[] = {
        0U, 'c', 0U, 'a', 0U, 'f', 0U, 0xe9U, 0xd8U, 0x3dU, 0xdeU, 0x42U
    };
    static const unsigned char cp1252[] = {0x80U, ' ', 0x93U, 'x', 0x94U};
    static const char cp1252_utf8[] = "\342\202\254 \342\200\234x\342\200\235";
    static const unsigned char bad_surrogate[] = {0x00U, 0xd8U, 'x', 0U};
    static const char graphemes[] = "e\314\201\360\237\221\251\342\200\215\360\237\222\273";
    static const char words[] = "can\'t cafe\314\201";
    char *utf8 = 0;
    unsigned char *encoded = 0;
    size_t length = 0U;
    RtGraphemeCluster cluster;
    RtWordSpan span;

    if (!rt_unicode_normalized_equal(composed, sizeof(composed) - 1U, decomposed, sizeof(decomposed) - 1U, 0)) return 1;
    if (!rt_unicode_normalized_equal(marks_a, sizeof(marks_a) - 1U, marks_b, sizeof(marks_b) - 1U, 0)) return 2;
    if (!rt_unicode_normalized_equal(hangul, sizeof(hangul) - 1U, jamo, sizeof(jamo) - 1U, 0)) return 3;

    if (rt_transcode_to_utf8(utf16le, sizeof(utf16le), RT_TEXT_ENCODING_UTF16, &utf8, &length) != 0) return 4;
    if (!bytes_equal(utf8, length, utf8_text, sizeof(utf8_text) - 1U)) return 5;
    rt_free(utf8);

    if (rt_transcode_from_utf8(utf8_text, sizeof(utf8_text) - 1U, RT_TEXT_ENCODING_UTF16BE, &encoded, &length) != 0) return 6;
    if (!bytes_equal(encoded, length, utf16be_expected, sizeof(utf16be_expected))) return 7;
    rt_free(encoded);

    if (rt_transcode_to_utf8(cp1252, sizeof(cp1252), RT_TEXT_ENCODING_WINDOWS_1252, &utf8, &length) != 0) return 8;
    if (!bytes_equal(utf8, length, cp1252_utf8, sizeof(cp1252_utf8) - 1U)) return 9;
    rt_free(utf8);
    if (rt_transcode_from_utf8(cp1252_utf8, sizeof(cp1252_utf8) - 1U, RT_TEXT_ENCODING_WINDOWS_1252, &encoded, &length) != 0) return 10;
    if (!bytes_equal(encoded, length, cp1252, sizeof(cp1252))) return 11;
    rt_free(encoded);
    if (rt_transcode_to_utf8(bad_surrogate, sizeof(bad_surrogate), RT_TEXT_ENCODING_UTF16LE, &utf8, &length) == 0) return 12;

    if (rt_grapheme_next(graphemes, sizeof(graphemes) - 1U, 0U, &cluster) != 0 || cluster.end != 3U) return 13;
    if (rt_grapheme_next(graphemes, sizeof(graphemes) - 1U, cluster.end, &cluster) != 0 || cluster.end != sizeof(graphemes) - 1U) return 14;

    if (rt_word_next(words, sizeof(words) - 1U, 0U, &span) != 0 || !span.is_word || span.end != 5U) return 15;
    if (rt_unicode_is_word_boundary(words, sizeof(words) - 1U, 3U)) return 16;
    if (!rt_unicode_is_word_boundary(words, sizeof(words) - 1U, 5U)) return 17;
    return 0;
}
EOF

cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/shared \
    "$WORK_DIR/unicode_runtime_test.c" \
    src/shared/runtime/unicode.c src/shared/runtime/unicode_utf8.c \
    -o "$WORK_DIR/unicode_runtime_test"
assert_command_succeeds "$WORK_DIR/unicode_runtime_test"
