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

"${TEST_BIN_DIR}/readapk" "$apk_path" > "$WORK_DIR/summary.txt"
assert_file_contains "$WORK_DIR/summary.txt" 'Type: Android APK / ZIP archive' "readapk did not classify APK-like ZIP"
assert_file_contains "$WORK_DIR/summary.txt" 'Entries: 8' "readapk did not count entries"
assert_file_contains "$WORK_DIR/summary.txt" 'AndroidManifest.xml: 1' "readapk did not count manifest"
assert_file_contains "$WORK_DIR/summary.txt" 'resources.arsc: 1' "readapk did not count resources.arsc"
assert_file_contains "$WORK_DIR/summary.txt" 'DEX files: 1' "readapk did not count DEX entries"
assert_file_contains "$WORK_DIR/summary.txt" 'Native libraries: 1' "readapk did not count native libraries"
assert_file_contains "$WORK_DIR/summary.txt" 'Payload size: compressed 0, uncompressed 0' "readapk summary did not include payload size overview"
assert_file_contains "$WORK_DIR/summary.txt" 'Native ABIs: arm64-v8a 1' "readapk summary did not include native ABI overview"
assert_file_contains "$WORK_DIR/summary.txt" 'APK v1 signature files: 1 cert, 1 manifest' "readapk did not count v1 signature files"
assert_file_contains "$WORK_DIR/summary.txt" 'APK Signing Block: yes offset' "readapk did not detect APK Signing Block"
assert_file_contains "$WORK_DIR/summary.txt" 'v2 yes' "readapk did not detect APK signature v2 ID"

"${TEST_BIN_DIR}/readapk" --dates "$apk_path" > "$WORK_DIR/dates.txt"
assert_file_contains "$WORK_DIR/dates.txt" 'Dates:' "readapk --dates did not print header"
assert_file_contains "$WORK_DIR/dates.txt" 'unknown  AndroidManifest.xml' "readapk --dates did not print manifest timestamp"

"${TEST_BIN_DIR}/readapk" --files-detail "$apk_path" > "$WORK_DIR/files-detail.txt"
assert_file_contains "$WORK_DIR/files-detail.txt" 'Files:' "readapk --files-detail did not print header"
assert_file_contains "$WORK_DIR/files-detail.txt" 'manifest: compressed 0, uncompressed 0, method stored, AndroidManifest.xml' "readapk --files-detail did not classify manifest"

"${TEST_BIN_DIR}/readapk" --resources-detail "$apk_path" > "$WORK_DIR/resources-detail.txt"
assert_file_contains "$WORK_DIR/resources-detail.txt" 'Resource files:' "readapk --resources-detail did not print resource file header"
assert_file_contains "$WORK_DIR/resources-detail.txt" 'table: 0 bytes resources.arsc' "readapk --resources-detail did not include resources.arsc"
assert_file_contains "$WORK_DIR/resources-detail.txt" 'res: 0 bytes res/layout/main.xml' "readapk --resources-detail did not list res/ entries"

"${TEST_BIN_DIR}/readapk" --code-detail "$apk_path" > "$WORK_DIR/code-detail.txt"
assert_file_contains "$WORK_DIR/code-detail.txt" 'Code detail:' "readapk --code-detail did not print header"
assert_file_contains "$WORK_DIR/code-detail.txt" 'DEX: classes.dex is not a DEX file' "readapk --code-detail did not inspect DEX entries"
assert_file_contains "$WORK_DIR/code-detail.txt" 'Native library: lib/arm64-v8a/libx.so is not ELF' "readapk --code-detail did not inspect native entries"

"${TEST_BIN_DIR}/readapk" --capabilities "$apk_path" > "$WORK_DIR/capabilities.txt"
assert_file_contains "$WORK_DIR/capabilities.txt" 'Capabilities: manifest is not Android binary XML' "readapk --capabilities did not inspect manifest"

"${TEST_BIN_DIR}/readapk" --security "$apk_path" > "$WORK_DIR/security.txt"
assert_file_contains "$WORK_DIR/security.txt" 'Security: manifest is not Android binary XML' "readapk --security did not inspect manifest"
assert_file_contains "$WORK_DIR/security.txt" 'Validation:' "readapk --security did not include ZIP validation"
assert_file_contains "$WORK_DIR/security.txt" 'Signatures:' "readapk --security did not include signature details"

"${TEST_BIN_DIR}/readapk" -l "$apk_path" > "$WORK_DIR/list.txt"
assert_file_contains "$WORK_DIR/list.txt" 'offset	method	compressed	uncompressed	crc32	flags	name' "readapk list header changed"
assert_file_contains "$WORK_DIR/list.txt" 'stored	0	0	00000000	--U-	AndroidManifest.xml' "readapk did not list manifest entry"
assert_file_contains "$WORK_DIR/list.txt" 'lib/arm64-v8a/libx.so' "readapk did not list native library"

"${TEST_BIN_DIR}/readapk" --json -a "$apk_path" > "$WORK_DIR/json.txt"
assert_file_contains "$WORK_DIR/json.txt" '"event":"apk_entry"' "readapk --json did not emit entries"
assert_file_contains "$WORK_DIR/json.txt" '"event":"apk_summary"' "readapk --json did not emit summary"
assert_file_contains "$WORK_DIR/json.txt" '"probable_apk":true' "readapk --json did not mark probable APK"
assert_file_contains "$WORK_DIR/json.txt" '"crc32":"00000000"' "readapk --json did not emit CRC32 text"
assert_file_contains "$WORK_DIR/json.txt" '"apk_signature_v2":true' "readapk --json did not report v2 signing block"

"${TEST_BIN_DIR}/readapk" --verify "$apk_path" > "$WORK_DIR/verify.txt"
assert_file_contains "$WORK_DIR/verify.txt" 'Validation:' "readapk --verify did not print validation header"
assert_file_contains "$WORK_DIR/verify.txt" 'checked entries: 8' "readapk --verify did not count entries"
assert_file_contains "$WORK_DIR/verify.txt" 'structural errors: 0' "readapk --verify reported errors for fixture"
assert_file_contains "$WORK_DIR/verify.txt" 'name mismatches: 0' "readapk --verify reported name mismatches for fixture"

"${TEST_BIN_DIR}/readapk" --manifest "$apk_path" > "$WORK_DIR/manifest.txt"
assert_file_contains "$WORK_DIR/manifest.txt" 'Manifest: AndroidManifest.xml is not Android binary XML' "readapk --manifest did not inspect manifest entry"

extract_path="$WORK_DIR/extract"
"${TEST_BIN_DIR}/readapk" --extract-manifest "$extract_path/manifest" --extract-dex "$extract_path/dex" --extract-native "$extract_path/native" --extract-signatures "$extract_path/signatures" "$apk_path" > "$WORK_DIR/extract.txt"
assert_file_contains "$WORK_DIR/extract.txt" 'extracted ' "readapk extraction modes did not report extracted files"
assert_file_contains "$extract_path/manifest/AndroidManifest.txt" 'Manifest: AndroidManifest.xml is not Android binary XML' "readapk --extract-manifest did not write decoded manifest text"
if [ ! -f "$extract_path/dex/classes.dex" ]; then
    fail "readapk --extract-dex did not preserve DEX entry path"
fi
if [ ! -f "$extract_path/native/lib/arm64-v8a/libx.so" ]; then
    fail "readapk --extract-native did not preserve native library entry path"
fi
if [ ! -f "$extract_path/signatures/META-INF/CERT.SF" ]; then
    fail "readapk --extract-signatures did not extract META-INF signature files"
fi
if [ ! -f "$extract_path/signatures/APKSigningBlock.bin" ]; then
    fail "readapk --extract-signatures did not extract APK Signing Block"
fi

if command -v python3 >/dev/null 2>&1; then
    resolved_apk_path="$WORK_DIR/resolved-manifest.apk"
    python3 - <<'PY' "$resolved_apk_path"
import struct
import sys
import zipfile

out_path = sys.argv[1]

def u16(value):
    return struct.pack('<H', value)

def u32(value):
    return struct.pack('<I', value)

def chunk(chunk_type, header_size, body):
    return u16(chunk_type) + u16(header_size) + u32(8 + len(body)) + body

def utf8_string_pool(strings):
    offsets = []
    payload = bytearray()
    for text in strings:
        encoded = text.encode('utf-8')
        offsets.append(len(payload))
        payload.append(len(text))
        payload.append(len(encoded))
        payload.extend(encoded)
        payload.append(0)
    while len(payload) % 4:
        payload.append(0)
    header_size = 28
    strings_start = header_size + 4 * len(strings)
    body = b''.join(u32(value) for value in (len(strings), 0, 0x100, strings_start, 0))
    body += b''.join(u32(offset) for offset in offsets)
    body += payload
    return chunk(0x0001, header_size, body)

def axml_manifest():
    strings = ['application', 'label']
    string_pool = utf8_string_pool(strings)
    node = u32(0) + u32(0xffffffff)
    ext = u32(0xffffffff) + u32(0) + u16(20) + u16(20) + u16(1) + u16(0) + u16(0) + u16(0)
    attr = u32(0xffffffff) + u32(1) + u32(0xffffffff) + u16(8) + bytes([0, 0x01]) + u32(0x7f010000)
    start = chunk(0x0102, 16, node + ext + attr)
    body = string_pool + start
    return u16(0x0003) + u16(8) + u32(8 + len(body)) + body

def resources_table():
    global_strings = utf8_string_pool(['Synthetic Clock'])
    type_strings = utf8_string_pool(['label'])
    key_strings = utf8_string_pool(['app_name'])
    entry = u16(8) + u16(0) + u32(0) + u16(8) + bytes([0, 0x03]) + u32(0)
    type_header_size = 84
    entries_start = type_header_size + 4
    type_body = bytes([1, 0, 0, 0]) + u32(1) + u32(entries_start) + u32(64) + bytes(60) + u32(0) + entry
    type_chunk = chunk(0x0201, type_header_size, type_body)
    package_header_size = 288
    type_strings_offset = package_header_size
    key_strings_offset = package_header_size + len(type_strings)
    package_size = package_header_size + len(type_strings) + len(key_strings) + len(type_chunk)
    package_header = u16(0x0200) + u16(package_header_size) + u32(package_size) + u32(0x7f)
    package_header += 'test'.encode('utf-16le') + bytes(256 - 8)
    package_header += u32(type_strings_offset) + u32(0) + u32(key_strings_offset) + u32(0) + u32(0)
    package = package_header + type_strings + key_strings + type_chunk
    body = u32(1) + global_strings + package
    return u16(0x0002) + u16(12) + u32(12 + len(global_strings) + len(package)) + body

with zipfile.ZipFile(out_path, 'w') as archive:
    archive.writestr('AndroidManifest.xml', axml_manifest(), compress_type=zipfile.ZIP_STORED)
    archive.writestr('resources.arsc', resources_table(), compress_type=zipfile.ZIP_STORED)
PY
    "${TEST_BIN_DIR}/readapk" --manifest "$resolved_apk_path" > "$WORK_DIR/resolved-manifest.txt"
    assert_file_contains "$WORK_DIR/resolved-manifest.txt" 'application label: Synthetic Clock' "readapk --manifest did not resolve resources.arsc string references"
    "${TEST_BIN_DIR}/readapk" --extract-resource 0x7f010000 "$resolved_apk_path" > "$WORK_DIR/resource.txt"
    assert_file_contains "$WORK_DIR/resource.txt" '0x7f010000: Synthetic Clock' "readapk --extract-resource did not print resolved resource value"
else
    note "python3 not available; skipping readapk resource-resolution fixture"
fi

"${TEST_BIN_DIR}/readapk" --resources "$apk_path" > "$WORK_DIR/resources.txt"
assert_file_contains "$WORK_DIR/resources.txt" 'resources.arsc: not an Android resource table' "readapk --resources did not inspect resources entry"

"${TEST_BIN_DIR}/readapk" --dex "$apk_path" > "$WORK_DIR/dex.txt"
assert_file_contains "$WORK_DIR/dex.txt" 'DEX: classes.dex is not a DEX file' "readapk --dex did not inspect DEX entry"

"${TEST_BIN_DIR}/readapk" --native "$apk_path" > "$WORK_DIR/native.txt"
assert_file_contains "$WORK_DIR/native.txt" 'Native library: lib/arm64-v8a/libx.so is not ELF' "readapk --native did not inspect native library entry"

"${TEST_BIN_DIR}/readapk" --signatures "$apk_path" > "$WORK_DIR/signatures.txt"
assert_file_contains "$WORK_DIR/signatures.txt" 'Signatures:' "readapk --signatures did not print signature header"
assert_file_contains "$WORK_DIR/signatures.txt" 'v2/v3/v3.1/source-stamp: yes/no/no/no' "readapk --signatures did not report signing block IDs"
assert_file_contains "$WORK_DIR/signatures.txt" 'v2 signers/signatures: 0/0' "readapk --signatures did not report v2 signer counts"
assert_file_contains "$WORK_DIR/signatures.txt" 'cryptographic verification: not implemented' "readapk --signatures did not state crypto validation status"

if "${TEST_BIN_DIR}/readapk" "$bad_path" > "$WORK_DIR/bad.out" 2> "$WORK_DIR/bad.err"; then
    fail "readapk should reject non-ZIP input"
fi
assert_file_contains "$WORK_DIR/bad.err" 'not a readable ZIP/APK' "readapk did not explain non-ZIP input"
