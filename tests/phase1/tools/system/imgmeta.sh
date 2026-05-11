#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup imgmeta

printf '\211PNG\015\012\032\012\000\000\000\015IHDR\000\000\000\001\000\000\000\001\010\006\000\000\000\037\025\304\211\000\000\000\007tEXtcomment\000\000\000\000\000\000\000\012IDAT\170\234c\000\001\000\000\005\000\001\015\012-\264\000\000\000\000IEND\256B`\202' > "$WORK_DIR/meta.png"
printf '\377\330\377\341\000\042Exif\000\000II\052\000\010\000\000\000\001\000\022\001\003\000\001\000\000\000\006\000\000\000\000\000\000\000\377\340\000\020JFIF\000\001\001\001\000\110\000\110\000\000\377\300\000\021\010\000\007\000\006\003\001\021\000\002\021\000\003\021\000\377\331' > "$WORK_DIR/meta.jpg"
printf 'RIFF\104\000\000\000WEBPVP8X\012\000\000\000\054\000\000\000\000\000\000\000\000\000EXIF\004\000\000\000ExifXMP \003\000\000\000xmp\000ICCP\004\000\000\000iccp' > "$WORK_DIR/meta.webp"

"$ROOT_DIR/build/imgmeta" show "$WORK_DIR/meta.jpg" > "$WORK_DIR/show-jpeg.out"
assert_file_contains "$WORK_DIR/show-jpeg.out" 'format: JPEG' "imgmeta show did not report JPEG format"
assert_file_contains "$WORK_DIR/show-jpeg.out" 'metadata: exif, orientation' "imgmeta show did not report JPEG EXIF metadata"
assert_file_contains "$WORK_DIR/show-jpeg.out" 'orientation: 6' "imgmeta show did not report JPEG orientation"

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

"$ROOT_DIR/build/imgmeta" strip -o "$WORK_DIR/clean.jpg" "$WORK_DIR/meta.jpg"
"$ROOT_DIR/build/imgmeta" show "$WORK_DIR/clean.jpg" > "$WORK_DIR/show-clean-jpeg.out"
if "$ROOT_DIR/build/grep" -q 'exif' "$WORK_DIR/show-clean-jpeg.out"; then
    fail "imgmeta strip should remove JPEG EXIF metadata"
fi
"$ROOT_DIR/build/imginfo" --plain "$WORK_DIR/clean.jpg" > "$WORK_DIR/clean-jpeg-info.out"
assert_file_contains "$WORK_DIR/clean-jpeg-info.out" 'image/jpeg' "imgmeta strip did not leave a recognizable JPEG"

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
