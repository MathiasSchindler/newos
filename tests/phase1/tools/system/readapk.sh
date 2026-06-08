#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup readapk

apk_path="$WORK_DIR/sample.apk"
central_path="$WORK_DIR/central.bin"
bad_path="$WORK_DIR/not.apk"
: > "$apk_path"
: > "$central_path"

apk_offset=0
central_size=0
entry_count=0

emit_byte_to() {
    target=$1
    byte_value=$2
    octal=$(printf '%03o' "$byte_value")
    printf "\\$octal" >> "$target"
}

emit_u16_to() {
    target=$1
    u16_value=$2
    emit_byte_to "$target" $((u16_value & 255))
    emit_byte_to "$target" $(((u16_value >> 8) & 255))
}

emit_u32_to() {
    target=$1
    u32_value=$2
    emit_byte_to "$target" $((u32_value & 255))
    emit_byte_to "$target" $(((u32_value >> 8) & 255))
    emit_byte_to "$target" $(((u32_value >> 16) & 255))
    emit_byte_to "$target" $(((u32_value >> 24) & 255))
}

emit_u64_to() {
    target=$1
    u64_value=$2
    emit_u32_to "$target" $((u64_value & 4294967295))
    emit_u32_to "$target" $(((u64_value >> 32) & 4294967295))
}

add_empty_entry() {
    name=$1
    name_len=${#name}
    local_offset=$apk_offset

    emit_u32_to "$apk_path" 67324752
    emit_u16_to "$apk_path" 20
    emit_u16_to "$apk_path" 2048
    emit_u16_to "$apk_path" 0
    emit_u16_to "$apk_path" 0
    emit_u16_to "$apk_path" 0
    emit_u32_to "$apk_path" 0
    emit_u32_to "$apk_path" 0
    emit_u32_to "$apk_path" 0
    emit_u16_to "$apk_path" "$name_len"
    emit_u16_to "$apk_path" 0
    printf '%s' "$name" >> "$apk_path"
    apk_offset=$((apk_offset + 30 + name_len))

    emit_u32_to "$central_path" 33639248
    emit_u16_to "$central_path" 20
    emit_u16_to "$central_path" 20
    emit_u16_to "$central_path" 2048
    emit_u16_to "$central_path" 0
    emit_u16_to "$central_path" 0
    emit_u16_to "$central_path" 0
    emit_u32_to "$central_path" 0
    emit_u32_to "$central_path" 0
    emit_u32_to "$central_path" 0
    emit_u16_to "$central_path" "$name_len"
    emit_u16_to "$central_path" 0
    emit_u16_to "$central_path" 0
    emit_u16_to "$central_path" 0
    emit_u16_to "$central_path" 0
    emit_u32_to "$central_path" 0
    emit_u32_to "$central_path" "$local_offset"
    printf '%s' "$name" >> "$central_path"
    central_size=$((central_size + 46 + name_len))
    entry_count=$((entry_count + 1))
}

add_empty_entry 'AndroidManifest.xml'
add_empty_entry 'classes.dex'
add_empty_entry 'resources.arsc'
add_empty_entry 'res/layout/main.xml'
add_empty_entry 'assets/readme.txt'
add_empty_entry 'lib/arm64-v8a/libx.so'
add_empty_entry 'META-INF/CERT.SF'
add_empty_entry 'META-INF/CERT.RSA'

emit_u64_to "$apk_path" 36
emit_u64_to "$apk_path" 4
emit_byte_to "$apk_path" 26
emit_byte_to "$apk_path" 135
emit_byte_to "$apk_path" 9
emit_byte_to "$apk_path" 113
emit_u64_to "$apk_path" 36
printf 'APK Sig Block 42' >> "$apk_path"
central_offset=$((apk_offset + 44))

cat "$central_path" >> "$apk_path"

emit_u32_to "$apk_path" 101010256
emit_u16_to "$apk_path" 0
emit_u16_to "$apk_path" 0
emit_u16_to "$apk_path" "$entry_count"
emit_u16_to "$apk_path" "$entry_count"
emit_u32_to "$apk_path" "$central_size"
emit_u32_to "$apk_path" "$central_offset"
emit_u16_to "$apk_path" 0

printf 'not an apk\n' > "$bad_path"

"$ROOT_DIR/build/readapk" "$apk_path" > "$WORK_DIR/summary.txt"
assert_file_contains "$WORK_DIR/summary.txt" 'Type: Android APK / ZIP archive' "readapk did not classify APK-like ZIP"
assert_file_contains "$WORK_DIR/summary.txt" 'Entries: 8' "readapk did not count entries"
assert_file_contains "$WORK_DIR/summary.txt" 'AndroidManifest.xml: 1' "readapk did not count manifest"
assert_file_contains "$WORK_DIR/summary.txt" 'resources.arsc: 1' "readapk did not count resources.arsc"
assert_file_contains "$WORK_DIR/summary.txt" 'DEX files: 1' "readapk did not count DEX entries"
assert_file_contains "$WORK_DIR/summary.txt" 'Native libraries: 1' "readapk did not count native libraries"
assert_file_contains "$WORK_DIR/summary.txt" 'APK v1 signature files: 1 cert, 1 manifest' "readapk did not count v1 signature files"
assert_file_contains "$WORK_DIR/summary.txt" 'APK Signing Block: yes offset' "readapk did not detect APK Signing Block"
assert_file_contains "$WORK_DIR/summary.txt" 'v2 yes' "readapk did not detect APK signature v2 ID"

"$ROOT_DIR/build/readapk" -l "$apk_path" > "$WORK_DIR/list.txt"
assert_file_contains "$WORK_DIR/list.txt" 'offset	method	compressed	uncompressed	crc32	flags	name' "readapk list header changed"
assert_file_contains "$WORK_DIR/list.txt" 'stored	0	0	00000000	--U-	AndroidManifest.xml' "readapk did not list manifest entry"
assert_file_contains "$WORK_DIR/list.txt" 'lib/arm64-v8a/libx.so' "readapk did not list native library"

"$ROOT_DIR/build/readapk" --json -a "$apk_path" > "$WORK_DIR/json.txt"
assert_file_contains "$WORK_DIR/json.txt" '"event":"apk_entry"' "readapk --json did not emit entries"
assert_file_contains "$WORK_DIR/json.txt" '"event":"apk_summary"' "readapk --json did not emit summary"
assert_file_contains "$WORK_DIR/json.txt" '"probable_apk":true' "readapk --json did not mark probable APK"
assert_file_contains "$WORK_DIR/json.txt" '"crc32":"00000000"' "readapk --json did not emit CRC32 text"
assert_file_contains "$WORK_DIR/json.txt" '"apk_signature_v2":true' "readapk --json did not report v2 signing block"

"$ROOT_DIR/build/readapk" --verify "$apk_path" > "$WORK_DIR/verify.txt"
assert_file_contains "$WORK_DIR/verify.txt" 'Validation:' "readapk --verify did not print validation header"
assert_file_contains "$WORK_DIR/verify.txt" 'checked entries: 8' "readapk --verify did not count entries"
assert_file_contains "$WORK_DIR/verify.txt" 'structural errors: 0' "readapk --verify reported errors for fixture"
assert_file_contains "$WORK_DIR/verify.txt" 'name mismatches: 0' "readapk --verify reported name mismatches for fixture"

"$ROOT_DIR/build/readapk" --manifest "$apk_path" > "$WORK_DIR/manifest.txt"
assert_file_contains "$WORK_DIR/manifest.txt" 'Manifest: AndroidManifest.xml is not Android binary XML' "readapk --manifest did not inspect manifest entry"

"$ROOT_DIR/build/readapk" --resources "$apk_path" > "$WORK_DIR/resources.txt"
assert_file_contains "$WORK_DIR/resources.txt" 'resources.arsc: not an Android resource table' "readapk --resources did not inspect resources entry"

"$ROOT_DIR/build/readapk" --dex "$apk_path" > "$WORK_DIR/dex.txt"
assert_file_contains "$WORK_DIR/dex.txt" 'DEX: classes.dex is not a DEX file' "readapk --dex did not inspect DEX entry"

"$ROOT_DIR/build/readapk" --native "$apk_path" > "$WORK_DIR/native.txt"
assert_file_contains "$WORK_DIR/native.txt" 'Native library: lib/arm64-v8a/libx.so is not ELF' "readapk --native did not inspect native library entry"

"$ROOT_DIR/build/readapk" --signatures "$apk_path" > "$WORK_DIR/signatures.txt"
assert_file_contains "$WORK_DIR/signatures.txt" 'Signatures:' "readapk --signatures did not print signature header"
assert_file_contains "$WORK_DIR/signatures.txt" 'v2/v3/v3.1/source-stamp: yes/no/no/no' "readapk --signatures did not report signing block IDs"
assert_file_contains "$WORK_DIR/signatures.txt" 'v2 signers/signatures: 0/0' "readapk --signatures did not report v2 signer counts"
assert_file_contains "$WORK_DIR/signatures.txt" 'cryptographic verification: not implemented' "readapk --signatures did not state crypto validation status"

if "$ROOT_DIR/build/readapk" "$bad_path" > "$WORK_DIR/bad.out" 2> "$WORK_DIR/bad.err"; then
    fail "readapk should reject non-ZIP input"
fi
assert_file_contains "$WORK_DIR/bad.err" 'not a readable ZIP/APK' "readapk did not explain non-ZIP input"
