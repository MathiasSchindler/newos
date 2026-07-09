#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup usbmon

fixture="$ROOT_DIR/tests/fixtures/usbmon.txt"

assert_command_succeeds "${TEST_BIN_DIR}/usbmon" --help > "$WORK_DIR/help.out" 2>&1
assert_file_contains "$WORK_DIR/help.out" '^Usage: usbmon ' "usbmon help did not print usage"

assert_command_succeeds "${TEST_BIN_DIR}/usbmon" --input "$fixture" --data > "$WORK_DIR/text.out"
assert_file_contains "$WORK_DIR/text.out" '^123 ffff8800 submit control in 1:5:0 status setup length 64 setup 80 06 0100 0000 0040$' "usbmon did not parse a control submission"
assert_file_contains "$WORK_DIR/text.out" '^140 ffff9900 submit bulk out 2:6:2 status -115 length 4 data deadbeef$' "usbmon did not join captured payload groups"
assert_file_contains "$WORK_DIR/text.out" '^150 ffffaa00 error interrupt in 2:6:1 status -32 length 0$' "usbmon did not parse an error record"
assert_file_contains "$WORK_DIR/text.out" '^160 ffffbb00 submit isochronous out 3:7:1 status -115:1:42 length 128 iso-frames 2 0:0:64 0:64:64 data 00010203$' "usbmon did not parse isochronous frame descriptors"

assert_command_succeeds "${TEST_BIN_DIR}/usbmon" --input "$fixture" --bus 2 --device 6 --endpoint 2 --type bulk --count 1 --data --json > "$WORK_DIR/json.out"
assert_file_contains "$WORK_DIR/json.out" '"schema":"newos.tool.v1"' "usbmon JSON omitted the shared schema"
assert_file_contains "$WORK_DIR/json.out" '"event":"transfer"' "usbmon JSON omitted the transfer event"
assert_file_contains "$WORK_DIR/json.out" '"transfer_type":"bulk"' "usbmon JSON type filter selected the wrong record"
assert_file_contains "$WORK_DIR/json.out" '"payload":"deadbeef"' "usbmon JSON omitted requested payload data"
[ "$(wc -l < "$WORK_DIR/json.out")" -eq 1 ] || fail "usbmon --count did not stop after one matching record"

assert_command_succeeds "${TEST_BIN_DIR}/usbmon" --input "$fixture" --raw --bus 1 > "$WORK_DIR/raw.out"
assert_file_contains "$WORK_DIR/raw.out" '^ffff8800 123 S Ci:1:005:0 s 80 06 0100 0000 0040 64 <$' "usbmon raw mode changed a matching record"
[ "$(wc -l < "$WORK_DIR/raw.out")" -eq 2 ] || fail "usbmon bus filter selected the wrong number of records"

assert_command_succeeds "${TEST_BIN_DIR}/usbmon" --input "$fixture" --type isochronous --json > "$WORK_DIR/iso-json.out"
assert_file_contains "$WORK_DIR/iso-json.out" '"iso_descriptor_count":2' "usbmon JSON omitted the isochronous descriptor count"
assert_file_contains "$WORK_DIR/iso-json.out" '"iso_descriptors":\["0:0:64","0:64:64"\]' "usbmon JSON omitted isochronous frame descriptors"

printf '%s\n' 'not a usbmon record' > "$WORK_DIR/malformed.txt"
if "${TEST_BIN_DIR}/usbmon" --input "$WORK_DIR/malformed.txt" > "$WORK_DIR/malformed.out" 2> "$WORK_DIR/malformed.err"; then
    fail "usbmon accepted a malformed replay record"
fi
assert_file_contains "$WORK_DIR/malformed.err" 'malformed input record' "usbmon did not diagnose malformed replay input"