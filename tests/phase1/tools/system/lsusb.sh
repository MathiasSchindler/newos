#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup lsusb

if command -v "${CC:-cc}" >/dev/null 2>&1; then
    assert_command_succeeds "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fno-builtin \
        -iquote "$ROOT_DIR/src/shared" \
        "$ROOT_DIR/tests/fixtures/usb_descriptor_test.c" \
        "$ROOT_DIR/src/shared/usb.c" \
        -o "$WORK_DIR/usb_descriptor_test"
    assert_command_succeeds "$WORK_DIR/usb_descriptor_test"
else
    note "phase1 system: USB descriptor parser check skipped; no C compiler found"
fi

assert_command_succeeds "${TEST_BIN_DIR}/lsusb" --help > "$WORK_DIR/help.out" 2>&1
assert_file_contains "$WORK_DIR/help.out" '^Usage: lsusb ' "lsusb help did not print usage"

if [ "$(uname -s 2>/dev/null || echo unknown)" = Linux ] && [ -d /dev/bus/usb ]; then
    first_device=$(find /dev/bus/usb -mindepth 2 -maxdepth 2 -type c -readable 2>/dev/null | LC_ALL=C sort | sed -n '1p')
    if [ -n "$first_device" ]; then
        assert_command_succeeds "${TEST_BIN_DIR}/lsusb" -p > "$WORK_DIR/list.out"
        assert_file_contains "$WORK_DIR/list.out" '^Bus [0-9][0-9]* Device [0-9][0-9]*: ID [0-9a-f][0-9a-f]*:[0-9a-f][0-9a-f]* class ' "Linux lsusb did not discover readable usbfs devices"
        assert_file_contains "$WORK_DIR/list.out" ' path /dev/bus/usb/' "Linux lsusb did not retain usbfs device paths"

        first_id=$(sed -n '1s/.* ID \([0-9a-f][0-9a-f]*:[0-9a-f][0-9a-f]*\) .*/\1/p' "$WORK_DIR/list.out")
        [ -n "$first_id" ] || fail "Linux lsusb output did not contain a filterable device ID"
        assert_command_succeeds "${TEST_BIN_DIR}/lsusb" -d "$first_id" > "$WORK_DIR/filter.out"
        assert_file_contains "$WORK_DIR/filter.out" " ID $first_id " "Linux lsusb vendor/product filter did not retain the selected device"

        first_bus=$(sed -n '1s/^Bus \([0-9][0-9]*\) Device \([0-9][0-9]*\):.*/\1/p' "$WORK_DIR/list.out")
        first_address=$(sed -n '1s/^Bus \([0-9][0-9]*\) Device \([0-9][0-9]*\):.*/\2/p' "$WORK_DIR/list.out")
        assert_command_succeeds "${TEST_BIN_DIR}/lsusb" -s "$first_bus:$first_address" > "$WORK_DIR/select.out"
        [ "$(wc -l < "$WORK_DIR/select.out")" -eq 1 ] || fail "Linux lsusb bus/device selector did not select one device"
        assert_command_succeeds "${TEST_BIN_DIR}/lsusb" -D "$first_device" > "$WORK_DIR/path-select.out"
        [ "$(wc -l < "$WORK_DIR/path-select.out")" -eq 1 ] || fail "Linux lsusb path selector did not select one device"

        assert_command_succeeds "${TEST_BIN_DIR}/lsusb" --json -s "$first_bus:$first_address" > "$WORK_DIR/json.out"
        assert_file_contains "$WORK_DIR/json.out" '"schema":"newos.tool.v1"' "Linux lsusb JSON omitted the shared schema"
        assert_file_contains "$WORK_DIR/json.out" '"event":"device"' "Linux lsusb JSON omitted the device event"
        assert_file_contains "$WORK_DIR/json.out" '"bus":' "Linux lsusb JSON omitted the bus number"

        assert_command_succeeds "${TEST_BIN_DIR}/lsusb" -v -a > "$WORK_DIR/verbose.out"
        assert_file_contains "$WORK_DIR/verbose.out" '^    \(configuration [0-9][0-9]*:.*\|descriptors unavailable\)$' "Linux lsusb verbose mode did not report descriptor status"
    else
        note "phase1 system: live Linux USB check skipped; no readable usbfs device nodes"
    fi
fi