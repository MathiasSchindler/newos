#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup imginfo
tab=$(printf '\t')

printf '\211PNG\015\012\032\012\000\000\000\015IHDR\000\000\000\002\000\000\000\003\010\006\000\000\000\000\000\000\000' > "$WORK_DIR/sample.png"
printf 'GIF89a\004\000\005\000\200\000\000' > "$WORK_DIR/sample.gif"
printf '\377\330\377\340\000\004\000\000\377\300\000\021\010\000\007\000\006\003\001\021\000\002\021\000\003\021\000\377\331' > "$WORK_DIR/sample.jpg"
printf 'II\052\000\010\000\000\000\004\000\000\001\004\000\001\000\000\000\011\000\000\000\001\001\004\000\001\000\000\000\012\000\000\000\002\001\003\000\001\000\000\000\010\000\000\000\025\001\003\000\001\000\000\000\003\000\000\000\000\000\000\000' > "$WORK_DIR/sample.tiff"
printf 'RIFF\036\000\000\000WEBPVP8X\012\000\000\000\020\000\000\000\013\000\000\014\000\000' > "$WORK_DIR/sample.webp"
printf 'BM\000\000\000\000\000\000\000\000\032\000\000\000\014\000\000\000\015\000\016\000\001\000\030\000' > "$WORK_DIR/sample.bmp"
printf 'not an image\n' > "$WORK_DIR/not-image.txt"

assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/sample.png")" "$WORK_DIR/sample.png${tab}png${tab}2${tab}3${tab}8${tab}4${tab}image/png" "imginfo did not parse PNG metadata"
assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/sample.gif")" "$WORK_DIR/sample.gif${tab}gif${tab}4${tab}5${tab}1${tab}3${tab}image/gif" "imginfo did not parse GIF metadata"
assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/sample.jpg")" "$WORK_DIR/sample.jpg${tab}jpeg${tab}6${tab}7${tab}8${tab}3${tab}image/jpeg" "imginfo did not parse JPEG metadata"
assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/sample.tiff")" "$WORK_DIR/sample.tiff${tab}tiff${tab}9${tab}10${tab}8${tab}3${tab}image/tiff" "imginfo did not parse TIFF metadata"
assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/sample.webp")" "$WORK_DIR/sample.webp${tab}webp${tab}12${tab}13${tab}-${tab}4${tab}image/webp" "imginfo did not parse WebP metadata"
assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/sample.bmp")" "$WORK_DIR/sample.bmp${tab}bmp${tab}13${tab}14${tab}24${tab}3${tab}image/bmp" "imginfo did not parse BMP metadata"
assert_text_equals "$($ROOT_DIR/build/imginfo --mime "$WORK_DIR/sample.png")" "$WORK_DIR/sample.png: image/png" "imginfo --mime did not print PNG MIME type"
assert_file_contains "$WORK_DIR/sample.png" 'PNG' "test fixture sanity check failed"

if "$ROOT_DIR/build/imginfo" "$WORK_DIR/not-image.txt" >/dev/null 2>&1; then
    fail "imginfo should reject unsupported image data"
fi

assert_text_equals "$(cat "$WORK_DIR/sample.gif" | "$ROOT_DIR/build/imginfo" --plain)" "stdin${tab}gif${tab}4${tab}5${tab}1${tab}3${tab}image/gif" "imginfo did not parse stdin"
