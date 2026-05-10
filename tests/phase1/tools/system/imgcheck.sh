#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup imgcheck
tab=$(printf '\t')

printf '\211PNG\015\012\032\012\000\000\000\015IHDR\000\000\000\001\000\000\000\001\010\006\000\000\000\037\025\304\211\000\000\000\012IDAT\170\234c\000\001\000\000\005\000\001\015\012-\264\000\000\000\000IEND\256B`\202' > "$WORK_DIR/valid.png"
printf '\211PNG\015\012\032\012\000\000\000\015IHDR\000\000\000\001\000\000\000\001\010\006\000\000\000\000\000\000\000\000\000\000\000IEND\256B`\202' > "$WORK_DIR/bad-crc.png"
printf '\211PNG\015\012\032\012\000\000\000\015IHDR\000\000\000\001\000\000\000\001\010\006\000\000\000\037\025\304\211\000\000\000\000IEND\256B`\202' > "$WORK_DIR/no-idat.png"
printf 'not an image\n' > "$WORK_DIR/not-image.txt"
printf 'GIF89a\001\000\001\000\200\000\000\000\000\000\377\377\377,\000\000\000\000\001\000\001\000\000\002\002D\001\000;' > "$WORK_DIR/sample.gif"
printf 'GIF89a\001\000\001\000\200\000\000\000\000\000\377\377\377,\000\000\000\000\001\000\001\000\000\002\002D' > "$WORK_DIR/truncated.gif"

assert_text_equals "$($ROOT_DIR/build/imgcheck --plain "$WORK_DIR/valid.png")" "$WORK_DIR/valid.png${tab}png${tab}ok${tab}valid PNG image" "imgcheck did not accept valid PNG"
assert_text_equals "$($ROOT_DIR/build/imgcheck --verbose "$WORK_DIR/valid.png")" "$WORK_DIR/valid.png: OK (png): valid PNG image" "imgcheck --verbose did not describe valid PNG"
assert_text_equals "$($ROOT_DIR/build/imgcheck --plain "$WORK_DIR/sample.gif")" "$WORK_DIR/sample.gif${tab}gif${tab}ok${tab}valid GIF image" "imgcheck did not accept valid GIF"

if "$ROOT_DIR/build/imgcheck" "$WORK_DIR/bad-crc.png" > "$WORK_DIR/bad-crc.out" 2>&1; then
    fail "imgcheck should reject PNG with bad CRC"
fi
assert_file_contains "$WORK_DIR/bad-crc.out" 'chunk CRC mismatch' "imgcheck did not report bad PNG CRC"

if "$ROOT_DIR/build/imgcheck" "$WORK_DIR/no-idat.png" > "$WORK_DIR/no-idat.out" 2>&1; then
    fail "imgcheck should reject PNG without IDAT"
fi
assert_file_contains "$WORK_DIR/no-idat.out" 'missing IDAT chunk' "imgcheck did not report missing IDAT"

if "$ROOT_DIR/build/imgcheck" "$WORK_DIR/truncated.gif" > "$WORK_DIR/truncated-gif.out" 2>&1; then
    fail "imgcheck should reject truncated GIF image data"
fi
assert_file_contains "$WORK_DIR/truncated-gif.out" 'image data sub-blocks are truncated' "imgcheck did not report truncated GIF image data"

if "$ROOT_DIR/build/imgcheck" "$WORK_DIR/not-image.txt" > "$WORK_DIR/not-image.out" 2>&1; then
    fail "imgcheck should reject unsupported data"
fi
assert_file_contains "$WORK_DIR/not-image.out" 'unsupported image format' "imgcheck did not report unsupported data"

cat "$WORK_DIR/valid.png" | "$ROOT_DIR/build/imgcheck" -q
if cat "$WORK_DIR/bad-crc.png" | "$ROOT_DIR/build/imgcheck" -q; then
    fail "imgcheck -q should reject invalid stdin"
fi
