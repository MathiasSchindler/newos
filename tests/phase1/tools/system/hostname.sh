#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup hostname

hostname_out=$("$ROOT_DIR/build/hostname" | tr -d '\r\n')
assert_nonempty_text "$hostname_out" "hostname output was empty"

uname_nodename_out=$("$ROOT_DIR/build/uname" -n | tr -d '\r\n')
assert_text_equals "$hostname_out" "$uname_nodename_out" "hostname and uname -n disagreed"

invalid_status=0
"$ROOT_DIR/build/hostname" 'bad name' >/dev/null 2>"$WORK_DIR/invalid.err" || invalid_status=$?
assert_exit_code "$invalid_status" '1' "hostname should reject invalid names before attempting the system call"
assert_file_contains "$WORK_DIR/invalid.err" '^hostname: invalid hostname$' "hostname did not explain the invalid-name rejection"

long_label=$(printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.example')
invalid_status=0
"$ROOT_DIR/build/hostname" "$long_label" >/dev/null 2>"$WORK_DIR/long-label.err" || invalid_status=$?
assert_exit_code "$invalid_status" '1' "hostname should reject labels longer than 63 bytes"
assert_file_contains "$WORK_DIR/long-label.err" '^hostname: invalid hostname$' "hostname did not explain the overlong-label rejection"
