#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup imgmeta

printf '\211PNG\015\012\032\012\000\000\000\015IHDR\000\000\000\001\000\000\000\001\010\006\000\000\000\037\025\304\211\000\000\000\007tEXtcomment\000\000\000\000\000\000\000\012IDAT\170\234c\000\001\000\000\005\000\001\015\012-\264\000\000\000\000IEND\256B`\202' > "$WORK_DIR/meta.png"
printf '\377\330\377\341\000\042Exif\000\000II\052\000\010\000\000\000\001\000\022\001\003\000\001\000\000\000\006\000\000\000\000\000\000\000\377\340\000\020JFIF\000\001\001\001\000\110\000\110\000\000\377\300\000\021\010\000\007\000\006\003\001\021\000\002\021\000\003\021\000\377\331' > "$WORK_DIR/meta.jpg"
printf '\377\330\377\353\000\056JP\000\001\000\000\000\014jumbjumdc2pac2pa.claimc2pa.signature\377\300\000\021\010\000\007\000\006\003\001\021\000\002\021\000\003\021\000\377\332\000\014\003\001\000\002\021\003\021\000\077\000\377\331' > "$WORK_DIR/c2pa.jpg"
printf 'RIFF\104\000\000\000WEBPVP8X\012\000\000\000\054\000\000\000\000\000\000\000\000\000EXIF\004\000\000\000ExifXMP \003\000\000\000xmp\000ICCP\004\000\000\000iccp' > "$WORK_DIR/meta.webp"
printf 'II\052\000\010\000\000\000\005\000\000\001\004\000\001\000\000\000\011\000\000\000\001\001\004\000\001\000\000\000\012\000\000\000\002\001\003\000\001\000\000\000\010\000\000\000\016\001\002\000\006\000\000\000\112\000\000\000\025\001\003\000\001\000\000\000\003\000\000\000\000\000\000\000hello\000' > "$WORK_DIR/meta.tiff"
printf 'II\053\000\010\000\000\000\020\000\000\000\000\000\000\000\005\000\000\000\000\000\000\000\000\001\004\000\001\000\000\000\000\000\000\000\011\000\000\000\000\000\000\000\001\001\004\000\001\000\000\000\000\000\000\000\012\000\000\000\000\000\000\000\002\001\003\000\001\000\000\000\000\000\000\000\010\000\000\000\000\000\000\000\016\001\002\000\011\000\000\000\000\000\000\000\174\000\000\000\000\000\000\000\025\001\003\000\001\000\000\000\000\000\000\000\003\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000bighello\000' > "$WORK_DIR/meta-bigtiff.tiff"

"$ROOT_DIR/build/imgmeta" show "$WORK_DIR/meta.jpg" > "$WORK_DIR/show-jpeg.out"
assert_file_contains "$WORK_DIR/show-jpeg.out" 'format: JPEG' "imgmeta show did not report JPEG format"
assert_file_contains "$WORK_DIR/show-jpeg.out" 'metadata: exif, orientation' "imgmeta show did not report JPEG EXIF metadata"
assert_file_contains "$WORK_DIR/show-jpeg.out" 'orientation: 6' "imgmeta show did not report JPEG orientation"

"$ROOT_DIR/build/imgmeta" show "$WORK_DIR/c2pa.jpg" > "$WORK_DIR/show-c2pa-jpeg.out"
assert_file_contains "$WORK_DIR/show-c2pa-jpeg.out" 'metadata: c2pa' "imgmeta show did not report C2PA metadata"
assert_file_contains "$WORK_DIR/show-c2pa-jpeg.out" 'c2pa-carrier: JPEG APP11 JUMBF' "imgmeta show did not report JPEG C2PA carrier"

if [ -f /home/mathias/c2pa/2.2/image/good/png/a.png ]; then
    "$ROOT_DIR/build/imgmeta" show /home/mathias/c2pa/2.2/image/good/png/a.png > "$WORK_DIR/show-c2pa-corpus-png.out"
    assert_file_contains "$WORK_DIR/show-c2pa-corpus-png.out" 'c2pa-cbor-boxes: 14' "imgmeta show did not parse C2PA CBOR boxes from corpus PNG"
    assert_file_contains "$WORK_DIR/show-c2pa-corpus-png.out" 'c2pa-cose-signatures: 3' "imgmeta show did not parse C2PA COSE signatures from corpus PNG"
    assert_file_contains "$WORK_DIR/show-c2pa-corpus-png.out" 'c2pa-x509-certificates: 6' "imgmeta show did not count C2PA X.509 certificates from corpus PNG"
    "$ROOT_DIR/build/imgmeta" show -v /home/mathias/c2pa/2.2/image/good/png/a.png > "$WORK_DIR/show-c2pa-corpus-png-verbose.out"
    assert_file_contains "$WORK_DIR/show-c2pa-corpus-png-verbose.out" 'c2pa-claim\[0\]:' "imgmeta show -v did not print C2PA claim details"
    assert_file_contains "$WORK_DIR/show-c2pa-corpus-png-verbose.out" 'instance-id:' "imgmeta show -v did not print C2PA claim instance IDs"
    assert_file_contains "$WORK_DIR/show-c2pa-corpus-png-verbose.out" 'created-assertions:' "imgmeta show -v did not print C2PA claim assertions"
    assert_file_contains "$WORK_DIR/show-c2pa-corpus-png-verbose.out" 'sha256=' "imgmeta show -v did not print C2PA assertion hashes"
fi

"$ROOT_DIR/build/imgmeta" strip -o "$WORK_DIR/clean.png" "$WORK_DIR/meta.png"
"$ROOT_DIR/build/imgcheck" --plain "$WORK_DIR/clean.png" > "$WORK_DIR/clean-png-check.out"
assert_file_contains "$WORK_DIR/clean-png-check.out" 'valid PNG image' "imgmeta strip did not leave a valid PNG"
if "$ROOT_DIR/build/grep" -q 'comment' "$WORK_DIR/clean.png"; then
    fail "imgmeta strip should remove PNG text chunks"
fi

"$ROOT_DIR/build/imgmeta" edit --set-text comment=updated -o "$WORK_DIR/edited.png" "$WORK_DIR/meta.png"
"$ROOT_DIR/build/imgcheck" --plain "$WORK_DIR/edited.png" > "$WORK_DIR/edited-png-check.out"
assert_file_contains "$WORK_DIR/edited-png-check.out" 'valid PNG image' "imgmeta edit did not leave a valid PNG"
if ! "$ROOT_DIR/build/strings" "$WORK_DIR/edited.png" | "$ROOT_DIR/build/grep" -q 'updated'; then
    fail "imgmeta edit should write updated PNG text metadata"
fi

"$ROOT_DIR/build/imgmeta" list-text "$WORK_DIR/edited.png" > "$WORK_DIR/list-text.out"
assert_file_contains "$WORK_DIR/list-text.out" 'comment' "imgmeta list-text should report PNG text keys"
assert_file_contains "$WORK_DIR/list-text.out" 'updated' "imgmeta list-text should report PNG text values"

"$ROOT_DIR/build/imgmeta" edit --remove-text comment -o "$WORK_DIR/removed.png" "$WORK_DIR/edited.png"
"$ROOT_DIR/build/imgmeta" list-text "$WORK_DIR/removed.png" > "$WORK_DIR/list-removed.out"
if "$ROOT_DIR/build/grep" -q 'comment' "$WORK_DIR/list-removed.out"; then
    fail "imgmeta edit --remove-text should remove PNG text metadata"
fi

"$ROOT_DIR/build/imgmeta" copy --from "$WORK_DIR/edited.png" -o "$WORK_DIR/copied-meta.png" "$WORK_DIR/clean.png"
"$ROOT_DIR/build/imgmeta" list-text "$WORK_DIR/copied-meta.png" > "$WORK_DIR/list-copied-meta.out"
assert_file_contains "$WORK_DIR/list-copied-meta.out" 'comment' "imgmeta copy --from should copy PNG text metadata keys"
assert_file_contains "$WORK_DIR/list-copied-meta.out" 'updated' "imgmeta copy --from should copy PNG text metadata values"

"$ROOT_DIR/build/imgmeta" edit --set-itxt caption=bonjour --language fr -o "$WORK_DIR/itxt.png" "$WORK_DIR/meta.png"
"$ROOT_DIR/build/imgmeta" list-text "$WORK_DIR/itxt.png" > "$WORK_DIR/list-itxt.out"
assert_file_contains "$WORK_DIR/list-itxt.out" 'iTXt' "imgmeta list-text should report PNG iTXt chunks"
assert_file_contains "$WORK_DIR/list-itxt.out" 'caption' "imgmeta list-text should report PNG iTXt keys"
assert_file_contains "$WORK_DIR/list-itxt.out" 'bonjour' "imgmeta list-text should report uncompressed PNG iTXt values"
"$ROOT_DIR/build/imgmeta" edit --remove-text caption -o "$WORK_DIR/itxt-removed.png" "$WORK_DIR/itxt.png"
"$ROOT_DIR/build/imgmeta" list-text "$WORK_DIR/itxt-removed.png" > "$WORK_DIR/list-itxt-removed.out"
if "$ROOT_DIR/build/grep" -q 'caption' "$WORK_DIR/list-itxt-removed.out"; then
    fail "imgmeta edit --remove-text should remove PNG iTXt metadata"
fi

"$ROOT_DIR/build/imgmeta" edit --set-itxt secret=hidden --compressed -o "$WORK_DIR/itxt-compressed.png" "$WORK_DIR/meta.png"
"$ROOT_DIR/build/imgcheck" --plain "$WORK_DIR/itxt-compressed.png" > "$WORK_DIR/itxt-compressed-check.out"
assert_file_contains "$WORK_DIR/itxt-compressed-check.out" 'valid PNG image' "imgmeta compressed iTXt edit did not leave a valid PNG"
"$ROOT_DIR/build/imgmeta" list-text "$WORK_DIR/itxt-compressed.png" > "$WORK_DIR/list-itxt-compressed.out"
assert_file_contains "$WORK_DIR/list-itxt-compressed.out" 'secret' "imgmeta list-text should report compressed PNG iTXt keys"
if "$ROOT_DIR/build/grep" -q 'hidden' "$WORK_DIR/list-itxt-compressed.out"; then
    fail "imgmeta list-text should not print compressed PNG iTXt values without decompression"
fi

"$ROOT_DIR/build/imgmeta" strip -o "$WORK_DIR/clean.jpg" "$WORK_DIR/meta.jpg"
"$ROOT_DIR/build/imgmeta" show "$WORK_DIR/clean.jpg" > "$WORK_DIR/show-clean-jpeg.out"
if "$ROOT_DIR/build/grep" -q 'exif' "$WORK_DIR/show-clean-jpeg.out"; then
    fail "imgmeta strip should remove JPEG EXIF metadata"
fi
"$ROOT_DIR/build/imginfo" --plain "$WORK_DIR/clean.jpg" > "$WORK_DIR/clean-jpeg-info.out"
assert_file_contains "$WORK_DIR/clean-jpeg-info.out" 'image/jpeg' "imgmeta strip did not leave a recognizable JPEG"
"$ROOT_DIR/build/imgmeta" strip -o "$WORK_DIR/clean-c2pa.jpg" "$WORK_DIR/c2pa.jpg"
"$ROOT_DIR/build/imgmeta" show "$WORK_DIR/clean-c2pa.jpg" > "$WORK_DIR/show-clean-c2pa-jpeg.out"
if "$ROOT_DIR/build/grep" -q 'c2pa' "$WORK_DIR/show-clean-c2pa-jpeg.out"; then
    fail "imgmeta strip should remove JPEG C2PA metadata"
fi

"$ROOT_DIR/build/imgmeta" show "$WORK_DIR/meta.webp" > "$WORK_DIR/show-webp.out"
assert_file_contains "$WORK_DIR/show-webp.out" 'metadata: exif, icc-profile, xmp' "imgmeta show did not report WebP metadata"
"$ROOT_DIR/build/imgmeta" strip -o "$WORK_DIR/clean.webp" "$WORK_DIR/meta.webp"
"$ROOT_DIR/build/imgmeta" show "$WORK_DIR/clean.webp" > "$WORK_DIR/show-clean-webp.out"
if "$ROOT_DIR/build/grep" -q 'exif\|icc-profile\|xmp' "$WORK_DIR/show-clean-webp.out"; then
    fail "imgmeta strip should remove WebP metadata chunks"
fi
if "$ROOT_DIR/build/grep" -q 'Exif\|xmp\|iccp' "$WORK_DIR/clean.webp"; then
    fail "imgmeta strip should remove WebP metadata payloads"
fi

"$ROOT_DIR/build/imgmeta" strip -o "$WORK_DIR/clean.tiff" "$WORK_DIR/meta.tiff"
"$ROOT_DIR/build/imginfo" --plain "$WORK_DIR/clean.tiff" > "$WORK_DIR/clean-tiff-info.out"
assert_file_contains "$WORK_DIR/clean-tiff-info.out" 'image/tiff' "imgmeta strip did not leave a recognizable TIFF"
assert_text_equals "$($ROOT_DIR/build/od -An -t x1 -j 8 -N 2 "$WORK_DIR/clean.tiff" | "$ROOT_DIR/build/tr" -d ' \n')" "0400" "imgmeta strip should remove TIFF metadata IFD entries"
if "$ROOT_DIR/build/strings" "$WORK_DIR/clean.tiff" | "$ROOT_DIR/build/grep" -q 'hello'; then
    fail "imgmeta strip should scrub TIFF metadata payload bytes"
fi

"$ROOT_DIR/build/imgmeta" strip -o "$WORK_DIR/clean-bigtiff.tiff" "$WORK_DIR/meta-bigtiff.tiff"
"$ROOT_DIR/build/imginfo" --plain "$WORK_DIR/clean-bigtiff.tiff" > "$WORK_DIR/clean-bigtiff-info.out"
assert_file_contains "$WORK_DIR/clean-bigtiff-info.out" 'image/tiff' "imgmeta strip did not leave a recognizable BigTIFF"
assert_text_equals "$($ROOT_DIR/build/od -An -t x1 -j 16 -N 8 "$WORK_DIR/clean-bigtiff.tiff" | "$ROOT_DIR/build/tr" -d ' \n')" "0400000000000000" "imgmeta strip should remove BigTIFF metadata IFD entries"
if "$ROOT_DIR/build/strings" "$WORK_DIR/clean-bigtiff.tiff" | "$ROOT_DIR/build/grep" -q 'bighello'; then
    fail "imgmeta strip should scrub BigTIFF metadata payload bytes"
fi

"$ROOT_DIR/build/imgmeta" copy -o "$WORK_DIR/copied.jpg" "$WORK_DIR/meta.jpg"
if ! "$ROOT_DIR/build/cmp" -s "$WORK_DIR/meta.jpg" "$WORK_DIR/copied.jpg"; then
    fail "imgmeta copy should preserve image bytes"
fi
"$ROOT_DIR/build/imgmeta" show "$WORK_DIR/copied.jpg" > "$WORK_DIR/show-copied-jpeg.out"
assert_file_contains "$WORK_DIR/show-copied-jpeg.out" 'metadata: exif, orientation' "imgmeta copy did not preserve JPEG metadata"

if "$ROOT_DIR/build/imgmeta" strip "$WORK_DIR/meta.png" >/dev/null 2>&1; then
    fail "imgmeta strip should require -o OUTPUT"
fi

if "$ROOT_DIR/build/imgmeta" copy "$WORK_DIR/meta.jpg" >/dev/null 2>&1; then
    fail "imgmeta copy should require -o OUTPUT"
fi

if "$ROOT_DIR/build/imgmeta" edit --set-text bad -o "$WORK_DIR/bad.png" "$WORK_DIR/meta.png" >/dev/null 2>&1; then
    fail "imgmeta edit should require KEY=VALUE text metadata"
fi
