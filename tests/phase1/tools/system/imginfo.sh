#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup imginfo
tab=$(printf '\t')

printf '\211PNG\015\012\032\012\000\000\000\015IHDR\000\000\000\002\000\000\000\003\010\006\000\000\000\000\000\000\000\000\000\000\011pHYs\000\000\016\304\000\000\016\304\001\000\000\000\000' > "$WORK_DIR/sample.png"
printf 'GIF89a\004\000\005\000\200\000\000' > "$WORK_DIR/sample.gif"
printf '\377\330\377\341\000\042Exif\000\000II\052\000\010\000\000\000\001\000\022\001\003\000\001\000\000\000\006\000\000\000\000\000\000\000\377\340\000\020JFIF\000\001\001\001\000\110\000\110\000\000\377\300\000\021\010\000\007\000\006\003\001\021\000\002\021\000\003\021\000\377\331' > "$WORK_DIR/sample.jpg"
printf '\377\330\377\353\000\056JP\000\001\000\000\000\014jumbjumdc2pac2pa.claimc2pa.signature\377\300\000\021\010\000\007\000\006\003\001\021\000\002\021\000\003\021\000\377\332\000\014\003\001\000\002\021\003\021\000\077\000\377\331' > "$WORK_DIR/c2pa.jpg"
printf '\377\330' > "$WORK_DIR/large-prefix.jpg"
i=0
while [ "$i" -lt 5 ]; do
    printf '\377\376\352\142' >> "$WORK_DIR/large-prefix.jpg"
    "$ROOT_DIR/build/yes" x | "$ROOT_DIR/build/head" -c 60000 >> "$WORK_DIR/large-prefix.jpg"
    i=$((i + 1))
done
printf '\377\300\000\021\010\000\007\000\006\003\001\021\000\002\021\000\003\021\000\377\331' >> "$WORK_DIR/large-prefix.jpg"
printf 'II\052\000\010\000\000\000\004\000\000\001\004\000\001\000\000\000\011\000\000\000\001\001\004\000\001\000\000\000\012\000\000\000\002\001\003\000\001\000\000\000\010\000\000\000\025\001\003\000\001\000\000\000\003\000\000\000\000\000\000\000' > "$WORK_DIR/sample.tiff"
printf 'II\053\000\010\000\000\000\020\000\000\000\000\000\000\000\006\000\000\000\000\000\000\000\000\001\004\000\001\000\000\000\000\000\000\000\013\000\000\000\000\000\000\000\001\001\004\000\001\000\000\000\000\000\000\000\014\000\000\000\000\000\000\000\002\001\003\000\001\000\000\000\000\000\000\000\010\000\000\000\000\000\000\000\003\001\003\000\001\000\000\000\000\000\000\000\001\000\000\000\000\000\000\000\006\001\003\000\001\000\000\000\000\000\000\000\002\000\000\000\000\000\000\000\025\001\003\000\001\000\000\000\000\000\000\000\003\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000' > "$WORK_DIR/sample-bigtiff.tiff"
printf 'RIFF\036\000\000\000WEBPVP8X\012\000\000\000\020\000\000\000\013\000\000\014\000\000' > "$WORK_DIR/sample.webp"
printf 'RIFFT\000\000\000WEBPVP8X\012\000\000\000\002\000\000\000\013\000\000\014\000\000ANIM\006\000\000\000\000\000\000\000\003\000ANMF\020\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\144\000\000\000ANMF\020\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\310\000\000\000' > "$WORK_DIR/animated.webp"
printf 'BM\000\000\000\000\000\000\000\000\032\000\000\000\014\000\000\000\015\000\016\000\001\000\030\000' > "$WORK_DIR/sample.bmp"
printf 'not an image\n' > "$WORK_DIR/not-image.txt"

assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/sample.png")" "$WORK_DIR/sample.png${tab}png${tab}2${tab}3${tab}8${tab}4${tab}image/png" "imginfo did not parse PNG metadata"
assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/sample.gif")" "$WORK_DIR/sample.gif${tab}gif${tab}4${tab}5${tab}1${tab}3${tab}image/gif" "imginfo did not parse GIF metadata"
assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/sample.jpg")" "$WORK_DIR/sample.jpg${tab}jpeg${tab}6${tab}7${tab}8${tab}3${tab}image/jpeg" "imginfo did not parse JPEG metadata"
assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/large-prefix.jpg")" "$WORK_DIR/large-prefix.jpg${tab}jpeg${tab}6${tab}7${tab}8${tab}3${tab}image/jpeg" "imginfo did not scan large JPEG metadata prefixes"
assert_text_equals "$(cat "$WORK_DIR/large-prefix.jpg" | "$ROOT_DIR/build/imginfo" --plain)" "stdin${tab}jpeg${tab}6${tab}7${tab}8${tab}3${tab}image/jpeg" "imginfo did not scan large JPEG metadata prefixes from stdin"
assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/sample.tiff")" "$WORK_DIR/sample.tiff${tab}tiff${tab}9${tab}10${tab}8${tab}3${tab}image/tiff" "imginfo did not parse TIFF metadata"
assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/sample-bigtiff.tiff")" "$WORK_DIR/sample-bigtiff.tiff${tab}tiff${tab}11${tab}12${tab}8${tab}3${tab}image/tiff" "imginfo did not parse BigTIFF metadata"
assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/sample.webp")" "$WORK_DIR/sample.webp${tab}webp${tab}12${tab}13${tab}-${tab}4${tab}image/webp" "imginfo did not parse WebP metadata"
assert_text_equals "$($ROOT_DIR/build/imginfo --plain "$WORK_DIR/sample.bmp")" "$WORK_DIR/sample.bmp${tab}bmp${tab}13${tab}14${tab}24${tab}3${tab}image/bmp" "imginfo did not parse BMP metadata"
assert_text_equals "$($ROOT_DIR/build/imginfo --mime "$WORK_DIR/sample.png")" "$WORK_DIR/sample.png: image/png" "imginfo --mime did not print PNG MIME type"
assert_text_equals "$($ROOT_DIR/build/imginfo --canonical-ext "$WORK_DIR/sample.png")" "png" "imginfo --canonical-ext did not print PNG extension"
assert_file_contains "$WORK_DIR/sample.png" 'PNG' "test fixture sanity check failed"

"$ROOT_DIR/build/imginfo" --json "$WORK_DIR/sample.png" > "$WORK_DIR/png-json.txt"
assert_file_contains "$WORK_DIR/png-json.txt" '"canonical_extension":"png"' "imginfo --json did not report canonical extension"
assert_file_contains "$WORK_DIR/png-json.txt" '"width":2' "imginfo --json did not report width"

"$ROOT_DIR/build/imginfo" --details "$WORK_DIR/sample.png" > "$WORK_DIR/png-details.txt"
assert_file_contains "$WORK_DIR/png-details.txt" 'variant: PNG' "imginfo --details did not print PNG variant"
assert_file_contains "$WORK_DIR/png-details.txt" 'color: truecolor-alpha' "imginfo --details did not print PNG color model"
assert_file_contains "$WORK_DIR/png-details.txt" 'compression: deflate' "imginfo --details did not print PNG compression"
assert_file_contains "$WORK_DIR/png-details.txt" 'density: 3780x3780 pixels/meter' "imginfo --details did not print PNG density"
assert_file_contains "$WORK_DIR/png-details.txt" 'properties: alpha' "imginfo --details did not print PNG properties"

"$ROOT_DIR/build/imginfo" --details "$WORK_DIR/sample.jpg" > "$WORK_DIR/jpeg-details.txt"
assert_file_contains "$WORK_DIR/jpeg-details.txt" 'density: 72x72 dpi' "imginfo --details did not print JPEG JFIF density"
assert_file_contains "$WORK_DIR/jpeg-details.txt" 'orientation: 6 (rotated 90 clockwise)' "imginfo --details did not print JPEG EXIF orientation"
assert_file_contains "$WORK_DIR/jpeg-details.txt" 'properties: exif, orientation' "imginfo --details did not print JPEG metadata properties"

"$ROOT_DIR/build/imginfo" --details "$WORK_DIR/c2pa.jpg" > "$WORK_DIR/c2pa-jpeg-details.txt"
assert_file_contains "$WORK_DIR/c2pa-jpeg-details.txt" 'properties: c2pa' "imginfo --details did not report C2PA metadata"
assert_file_contains "$WORK_DIR/c2pa-jpeg-details.txt" 'c2pa-carrier: JPEG APP11 JUMBF' "imginfo --details did not report JPEG C2PA carrier"
assert_file_contains "$WORK_DIR/c2pa-jpeg-details.txt" 'c2pa-claims: 1' "imginfo --details did not count C2PA claims"
"$ROOT_DIR/build/imginfo" --json "$WORK_DIR/c2pa.jpg" > "$WORK_DIR/c2pa-jpeg-json.txt"
assert_file_contains "$WORK_DIR/c2pa-jpeg-json.txt" '"c2pa":{"present":true' "imginfo --json did not report C2PA presence"
assert_file_contains "$WORK_DIR/c2pa-jpeg-json.txt" '"signature_count":1' "imginfo --json did not count C2PA signatures"

if [ -f /home/mathias/c2pa/2.2/image/good/jpeg/a.jpg ]; then
    "$ROOT_DIR/build/imginfo" --details /home/mathias/c2pa/2.2/image/good/jpeg/a.jpg > "$WORK_DIR/c2pa-corpus-jpeg-details.txt"
    assert_file_contains "$WORK_DIR/c2pa-corpus-jpeg-details.txt" 'c2pa-cbor-boxes: 20' "imginfo did not parse C2PA CBOR boxes from corpus JPEG"
    assert_file_contains "$WORK_DIR/c2pa-corpus-jpeg-details.txt" 'c2pa-cose-signatures: 4' "imginfo did not parse C2PA COSE signatures from corpus JPEG"
    assert_file_contains "$WORK_DIR/c2pa-corpus-jpeg-details.txt" 'c2pa-x509-certificates: 8' "imginfo did not count C2PA X.509 certificates from corpus JPEG"
    assert_file_contains "$WORK_DIR/c2pa-corpus-jpeg-details.txt" 'c2pa-signature-verification: unsupported' "imginfo should report unsupported C2PA signature verification"
fi

"$ROOT_DIR/build/imginfo" --details "$WORK_DIR/sample.webp" > "$WORK_DIR/webp-details.txt"
assert_file_contains "$WORK_DIR/webp-details.txt" 'variant: extended WebP' "imginfo --details did not print WebP variant"
assert_file_contains "$WORK_DIR/webp-details.txt" 'properties: alpha' "imginfo --details did not print WebP alpha property"

"$ROOT_DIR/build/imginfo" --details "$WORK_DIR/animated.webp" > "$WORK_DIR/animated-webp-details.txt"
assert_file_contains "$WORK_DIR/animated-webp-details.txt" 'frames: 2' "imginfo --details did not count animated WebP frames"
assert_file_contains "$WORK_DIR/animated-webp-details.txt" 'duration-ms: 300' "imginfo --details did not sum animated WebP duration"
assert_file_contains "$WORK_DIR/animated-webp-details.txt" 'loop-count: 3' "imginfo --details did not report animated WebP loop count"
assert_file_contains "$WORK_DIR/animated-webp-details.txt" 'properties: animated' "imginfo --details did not report animated WebP property"

cp "$WORK_DIR/sample.png" "$WORK_DIR/misleading.jpg"
"$ROOT_DIR/build/imginfo" "$WORK_DIR/misleading.jpg" > "$WORK_DIR/mismatch-out.txt" 2> "$WORK_DIR/mismatch-err.txt"
assert_file_contains "$WORK_DIR/mismatch-out.txt" 'PNG image' "imginfo mismatch warning should not suppress normal output"
assert_file_contains "$WORK_DIR/mismatch-err.txt" 'warning: file extension .jpg does not match detected png' "imginfo did not warn about mismatched extension"

mkdir "$WORK_DIR/nested"
cp "$WORK_DIR/sample.png" "$WORK_DIR/nested/inner.png"
"$ROOT_DIR/build/imginfo" --recursive --json "$WORK_DIR/nested" > "$WORK_DIR/recursive-json.txt"
assert_file_contains "$WORK_DIR/recursive-json.txt" 'inner.png' "imginfo --recursive did not visit nested image"

if "$ROOT_DIR/build/imginfo" "$WORK_DIR/not-image.txt" >/dev/null 2>&1; then
    fail "imginfo should reject unsupported image data"
fi

assert_text_equals "$(cat "$WORK_DIR/sample.gif" | "$ROOT_DIR/build/imginfo" --plain)" "stdin${tab}gif${tab}4${tab}5${tab}1${tab}3${tab}image/gif" "imginfo did not parse stdin"
