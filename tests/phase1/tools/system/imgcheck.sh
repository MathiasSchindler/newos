#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup imgcheck
tab=$(printf '\t')

printf '\211PNG\015\012\032\012\000\000\000\015IHDR\000\000\000\001\000\000\000\001\010\006\000\000\000\037\025\304\211\000\000\000\012IDAT\170\234c\000\001\000\000\005\000\001\015\012-\264\000\000\000\000IEND\256B`\202' > "$WORK_DIR/valid.png"
printf '\211PNG\015\012\032\012\000\000\000\015IHDR\000\000\000\001\000\000\000\001\010\006\000\000\000\037\025\304\211\000\000\000\012IDAT\170\234c\000\001\000\000\005\000\001\015\012-\264\000\000\000\007tIME\000\000\000\000\000\000\000\011\163\224\056\000\000\000\000IEND\256B`\202' > "$WORK_DIR/late-time.png"
printf '\211PNG\015\012\032\012\000\000\000\015IHDR\000\000\000\001\000\000\000\001\010\006\000\000\000\000\000\000\000\000\000\000\000IEND\256B`\202' > "$WORK_DIR/bad-crc.png"
printf '\211PNG\015\012\032\012\000\000\000\015IHDR\000\000\000\001\000\000\000\001\010\006\000\000\000\037\025\304\211\000\000\000\000IEND\256B`\202' > "$WORK_DIR/no-idat.png"
printf 'not an image\n' > "$WORK_DIR/not-image.txt"
printf 'GIF89a\001\000\001\000\200\000\000\000\000\000\377\377\377,\000\000\000\000\001\000\001\000\000\002\002D\001\000;' > "$WORK_DIR/sample.gif"
printf 'GIF89a\001\000\001\000\200\000\000\000\000\000\377\377\377,\000\000\000\000\001\000\001\000\000\002\002D' > "$WORK_DIR/truncated.gif"
printf '\377\330\377\340\000\020JFIF\000\001\001\001\000\110\000\110\000\000\377\300\000\021\010\000\007\000\006\003\001\021\000\002\021\000\003\021\000\377\332\000\014\003\001\000\002\021\003\021\000\077\000\377\331' > "$WORK_DIR/sample.jpg"
printf '\377\330\377\353\000\056JP\000\001\000\000\000\014jumbjumdc2pac2pa.claimc2pa.signature\377\300\000\021\010\000\007\000\006\003\001\021\000\002\021\000\003\021\000\377\332\000\014\003\001\000\002\021\003\021\000\077\000\377\331' > "$WORK_DIR/c2pa.jpg"
printf '\377\330\377\340\377\377JFIF\000' > "$WORK_DIR/bad-segment.jpg"
printf 'BM\072\000\000\000\000\000\000\000\066\000\000\000\050\000\000\000\001\000\000\000\001\000\000\000\001\000\030\000\000\000\000\000\004\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000' > "$WORK_DIR/sample.bmp"
printf 'BM\066\000\000\000\000\000\000\000\066\000\000\000\050\000\000\000\001\000\000\000\001\000\000\000\001\000\030\000\000\000\000\000\004\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000' > "$WORK_DIR/truncated.bmp"
printf 'II\052\000\010\000\000\000\004\000\000\001\004\000\001\000\000\000\011\000\000\000\001\001\004\000\001\000\000\000\012\000\000\000\002\001\003\000\001\000\000\000\010\000\000\000\025\001\003\000\001\000\000\000\003\000\000\000\000\000\000\000' > "$WORK_DIR/sample.tiff"
printf 'II\053\000\010\000\000\000\020\000\000\000\000\000\000\000\006\000\000\000\000\000\000\000\000\001\004\000\001\000\000\000\000\000\000\000\013\000\000\000\000\000\000\000\001\001\004\000\001\000\000\000\000\000\000\000\014\000\000\000\000\000\000\000\002\001\003\000\001\000\000\000\000\000\000\000\010\000\000\000\000\000\000\000\003\001\003\000\001\000\000\000\000\000\000\000\001\000\000\000\000\000\000\000\006\001\003\000\001\000\000\000\000\000\000\000\002\000\000\000\000\000\000\000\025\001\003\000\001\000\000\000\000\000\000\000\003\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000' > "$WORK_DIR/sample-bigtiff.tiff"
printf 'RIFF\056\000\000\000WEBPVP8X\012\000\000\000\002\000\000\000\000\000\000\000\000\000ANMF\020\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000' > "$WORK_DIR/sample.webp"

assert_text_equals "$($ROOT_DIR/build/imgcheck --plain "$WORK_DIR/valid.png")" "$WORK_DIR/valid.png${tab}png${tab}ok${tab}-${tab}valid PNG image" "imgcheck did not accept valid PNG"
assert_text_equals "$($ROOT_DIR/build/imgcheck --verbose "$WORK_DIR/valid.png")" "$WORK_DIR/valid.png: OK (png): valid PNG image" "imgcheck --verbose did not describe valid PNG"
assert_text_equals "$($ROOT_DIR/build/imgcheck --plain "$WORK_DIR/sample.jpg")" "$WORK_DIR/sample.jpg${tab}jpeg${tab}ok${tab}-${tab}valid JPEG image" "imgcheck did not accept valid JPEG"
assert_text_equals "$($ROOT_DIR/build/imgcheck --plain "$WORK_DIR/c2pa.jpg")" "$WORK_DIR/c2pa.jpg${tab}jpeg${tab}ok${tab}-${tab}valid JPEG image${tab}C2PA markers found; manifest store not recognized" "imgcheck did not report C2PA status"
assert_text_equals "$($ROOT_DIR/build/imgcheck --plain "$WORK_DIR/sample.gif")" "$WORK_DIR/sample.gif${tab}gif${tab}ok${tab}-${tab}valid GIF image" "imgcheck did not accept valid GIF"
assert_text_equals "$($ROOT_DIR/build/imgcheck --plain "$WORK_DIR/sample.bmp")" "$WORK_DIR/sample.bmp${tab}bmp${tab}ok${tab}-${tab}valid BMP image and pixel array" "imgcheck did not accept valid BMP"
assert_text_equals "$($ROOT_DIR/build/imgcheck --plain "$WORK_DIR/sample.tiff")" "$WORK_DIR/sample.tiff${tab}tiff${tab}ok${tab}-${tab}valid TIFF header and first IFD" "imgcheck did not validate TIFF first IFD"
assert_text_equals "$($ROOT_DIR/build/imgcheck --plain "$WORK_DIR/sample-bigtiff.tiff")" "$WORK_DIR/sample-bigtiff.tiff${tab}tiff${tab}ok${tab}-${tab}valid BigTIFF header and first IFD" "imgcheck did not validate BigTIFF first IFD"
assert_text_equals "$($ROOT_DIR/build/imgcheck --plain "$WORK_DIR/sample.webp")" "$WORK_DIR/sample.webp${tab}webp${tab}ok${tab}-${tab}valid WebP RIFF image" "imgcheck did not validate WebP RIFF image"

assert_file_contains "$WORK_DIR/late-time.png" 'PNG' "strict-mode fixture sanity check failed"
"$ROOT_DIR/build/imgcheck" --plain "$WORK_DIR/late-time.png" > "$WORK_DIR/late-time-default.out"
assert_file_contains "$WORK_DIR/late-time-default.out" 'valid PNG image' "imgcheck default mode should accept late ancillary PNG chunks"
if "$ROOT_DIR/build/imgcheck" --strict "$WORK_DIR/late-time.png" > "$WORK_DIR/late-time-strict.out" 2>&1; then
    fail "imgcheck --strict should reject late ancillary PNG chunks"
fi
assert_file_contains "$WORK_DIR/late-time-strict.out" 'strict PNG rejects ancillary chunks after IDAT' "imgcheck --strict did not report late ancillary PNG chunks"

"$ROOT_DIR/build/imgcheck" --json "$WORK_DIR/valid.png" > "$WORK_DIR/valid-json.out"
assert_file_contains "$WORK_DIR/valid-json.out" '"schema":"newos.tool.v1"' "imgcheck --json did not use the shared JSON envelope"
assert_file_contains "$WORK_DIR/valid-json.out" '"event":"image_check"' "imgcheck --json did not emit image_check events"
assert_file_contains "$WORK_DIR/valid-json.out" '"valid":true' "imgcheck --json did not report success"
assert_file_contains "$WORK_DIR/valid-json.out" '"failure_offset":null' "imgcheck --json did not report null success offset"
"$ROOT_DIR/build/imgcheck" --json "$WORK_DIR/c2pa.jpg" > "$WORK_DIR/c2pa-json.out"
assert_file_contains "$WORK_DIR/c2pa-json.out" '"c2pa":{"present":true' "imgcheck --json did not report C2PA presence"

if [ -f /home/mathias/c2pa/2.2/image/good/jpeg/a.jpg ]; then
    "$ROOT_DIR/build/imgcheck" --json /home/mathias/c2pa/2.2/image/good/jpeg/a.jpg > "$WORK_DIR/c2pa-corpus-json.out"
    assert_file_contains "$WORK_DIR/c2pa-corpus-json.out" '"cbor_valid":true' "imgcheck --json did not report valid C2PA CBOR"
    assert_file_contains "$WORK_DIR/c2pa-corpus-json.out" '"cose_valid":true' "imgcheck --json did not report valid C2PA COSE"
    assert_file_contains "$WORK_DIR/c2pa-corpus-json.out" '"signature_algorithm":"ES256"' "imgcheck --json did not report C2PA signature algorithm"
    assert_file_contains "$WORK_DIR/c2pa-corpus-json.out" '"trust_validation_supported":false' "imgcheck --json should report unsupported C2PA trust validation"
fi

mkdir "$WORK_DIR/nested"
cp "$WORK_DIR/valid.png" "$WORK_DIR/nested/inner.png"
"$ROOT_DIR/build/imgcheck" --recursive --json "$WORK_DIR/nested" > "$WORK_DIR/recursive-json.out"
assert_file_contains "$WORK_DIR/recursive-json.out" 'inner.png' "imgcheck --recursive did not visit nested image"
assert_file_contains "$WORK_DIR/recursive-json.out" '"valid":true' "imgcheck --recursive --json did not validate nested image"

if "$ROOT_DIR/build/imgcheck" "$WORK_DIR/bad-crc.png" > "$WORK_DIR/bad-crc.out" 2>&1; then
    fail "imgcheck should reject PNG with bad CRC"
fi
assert_file_contains "$WORK_DIR/bad-crc.out" 'chunk CRC mismatch' "imgcheck did not report bad PNG CRC"
assert_file_contains "$WORK_DIR/bad-crc.out" 'offset 8' "imgcheck did not report bad PNG CRC offset"

"$ROOT_DIR/build/imgcheck" --json "$WORK_DIR/bad-crc.png" > "$WORK_DIR/bad-crc-json.out" 2>&1 || true
assert_file_contains "$WORK_DIR/bad-crc-json.out" '"failure_offset":8' "imgcheck --json did not report bad PNG CRC offset"

if "$ROOT_DIR/build/imgcheck" "$WORK_DIR/no-idat.png" > "$WORK_DIR/no-idat.out" 2>&1; then
    fail "imgcheck should reject PNG without IDAT"
fi
assert_file_contains "$WORK_DIR/no-idat.out" 'missing IDAT chunk' "imgcheck did not report missing IDAT"

if "$ROOT_DIR/build/imgcheck" "$WORK_DIR/bad-segment.jpg" > "$WORK_DIR/bad-jpeg.out" 2>&1; then
    fail "imgcheck should reject JPEG with invalid segment length"
fi
assert_file_contains "$WORK_DIR/bad-jpeg.out" 'JPEG segment length exceeds file size' "imgcheck did not report invalid JPEG segment length"

if "$ROOT_DIR/build/imgcheck" "$WORK_DIR/truncated.gif" > "$WORK_DIR/truncated-gif.out" 2>&1; then
    fail "imgcheck should reject truncated GIF image data"
fi
assert_file_contains "$WORK_DIR/truncated-gif.out" 'image data sub-blocks are truncated' "imgcheck did not report truncated GIF image data"

if "$ROOT_DIR/build/imgcheck" "$WORK_DIR/truncated.bmp" > "$WORK_DIR/truncated-bmp.out" 2>&1; then
    fail "imgcheck should reject truncated BMP pixel data"
fi
assert_file_contains "$WORK_DIR/truncated-bmp.out" 'BMP pixel array is truncated' "imgcheck did not report truncated BMP pixel data"

if "$ROOT_DIR/build/imgcheck" "$WORK_DIR/not-image.txt" > "$WORK_DIR/not-image.out" 2>&1; then
    fail "imgcheck should reject unsupported data"
fi
assert_file_contains "$WORK_DIR/not-image.out" 'unsupported image format' "imgcheck did not report unsupported data"

cat "$WORK_DIR/valid.png" | "$ROOT_DIR/build/imgcheck" -q
if cat "$WORK_DIR/bad-crc.png" | "$ROOT_DIR/build/imgcheck" -q; then
    fail "imgcheck -q should reject invalid stdin"
fi
