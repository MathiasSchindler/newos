#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup c2pa

printf '\211PNG\015\012\032\012\000\000\000\015IHDR\000\000\000\001\000\000\000\001\010\006\000\000\000\037\025\304\211\000\000\000\012IDAT\170\234c\000\001\000\000\005\000\001\015\012-\264\000\000\000\000IEND\256B`\202' > "$WORK_DIR/base.png"

"$ROOT_DIR/build/c2pa" add --dev-key \
    --claim-generator "phase1 c2pa fixture" \
    --action "fictional.claim.taken_in_1899" \
    --action "fictional.claim.location_moon" \
    -o "$WORK_DIR/claimed.png" "$WORK_DIR/base.png"

"$ROOT_DIR/build/imgcheck" --plain "$WORK_DIR/claimed.png" > "$WORK_DIR/imgcheck.out"
assert_file_contains "$WORK_DIR/imgcheck.out" 'valid PNG image' "c2pa add did not leave a valid PNG"
assert_file_contains "$WORK_DIR/imgcheck.out" 'C2PA content hash validated' "c2pa add did not produce a verifiable content hash"

"$ROOT_DIR/build/imgmeta" show -v "$WORK_DIR/claimed.png" > "$WORK_DIR/imgmeta.out"
assert_file_contains "$WORK_DIR/imgmeta.out" 'c2pa-carrier: PNG caBX' "c2pa add did not write a PNG caBX carrier"
assert_file_contains "$WORK_DIR/imgmeta.out" 'algorithm: ES256' "c2pa add did not write an ES256 signature"
assert_file_contains "$WORK_DIR/imgmeta.out" 'generator: phase1 c2pa fixture' "c2pa add did not write v2 claim_generator_info"
assert_file_contains "$WORK_DIR/imgmeta.out" 'assertion-hash-algorithm: sha256' "c2pa add did not write claim-level hash algorithm"
if "$ROOT_DIR/build/grep" -q 'c2pa.hash.data sha256=cda160f6dbf0e55b822788dcec33fc8cb5b7f2a1b18ad951d655a8a39d382d02' "$WORK_DIR/imgmeta.out"; then
    fail "c2pa add should hash the c2pa.hash.data JUMBF box, not the raw assertion CBOR"
fi
if "$ROOT_DIR/build/grep" -q 'c2pa.actions.v2 sha256=d68fab5cc5ededf6d72f267104a3655e0346ac3b5fcee137cecf71625968a526' "$WORK_DIR/imgmeta.out"; then
    fail "c2pa add should hash the c2pa.actions.v2 JUMBF box, not the raw assertion CBOR"
fi
assert_file_contains "$WORK_DIR/imgmeta.out" '      - c2pa.created' "c2pa add should prepend c2pa.created before custom actions"
assert_file_contains "$WORK_DIR/imgmeta.out" '      - fictional.claim.taken_in_1899' "c2pa add did not keep the first custom action"
assert_file_contains "$WORK_DIR/imgmeta.out" '      - fictional.claim.location_moon' "c2pa add did not keep the second custom action"

"$ROOT_DIR/build/od" -A n -t x1 "$WORK_DIR/claimed.png" | "$ROOT_DIR/build/tr" -d ' \n' > "$WORK_DIR/claimed.hex"
if "$ROOT_DIR/build/grep" -q '6a617373657274696f6e73' "$WORK_DIR/claimed.hex"; then
    fail "c2pa add should not write the legacy bare assertions claim key in c2pa.claim.v2"
fi
if "$ROOT_DIR/build/grep" -q '6f636c61696d5f67656e657261746f72' "$WORK_DIR/claimed.hex"; then
    fail "c2pa add should not write the legacy claim_generator claim key in c2pa.claim.v2"
fi
assert_file_contains "$WORK_DIR/claimed.hex" '74636c61696d5f67656e657261746f725f696e666f' "c2pa add should write claim_generator_info in c2pa.claim.v2"
assert_file_contains "$WORK_DIR/claimed.hex" '6a756d646332706100110010800000aa00389b71036332706100' "C2PA store JUMBF description is missing UUID/toggles/label"
assert_file_contains "$WORK_DIR/claimed.hex" '6a756d6463326d6100110010800000aa00389b710375726e3a633270613a6e65776f732d64657600' "C2PA manifest JUMBF description is missing UUID/toggles/label"
assert_file_contains "$WORK_DIR/claimed.hex" '6a756d6463626f7200110010800000aa00389b7103633270612e616374696f6e732e763200' "C2PA action assertion JUMBF description is missing UUID/toggles/label"