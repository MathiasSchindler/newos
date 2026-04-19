#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup dmesg

"$ROOT_DIR/build/dmesg" --help > "$WORK_DIR/dmesg_help.out"
assert_file_contains "$WORK_DIR/dmesg_help.out" '^Usage: .*dmesg' "dmesg --help did not print usage"

dmesg_status=0
"$ROOT_DIR/build/dmesg" > "$WORK_DIR/dmesg.out" 2>"$WORK_DIR/dmesg.err" || dmesg_status=$?
if [ "$dmesg_status" -eq 0 ]; then
    assert_file_contains "$WORK_DIR/dmesg.out" '.' "dmesg succeeded but produced no output"
else
    [ -s "$WORK_DIR/dmesg.err" ] || fail "dmesg failed without any diagnostic output"
fi
