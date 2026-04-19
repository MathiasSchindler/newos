#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/truncate"
note "phase1 filesystem truncate"

printf 'truncate\n' > "$WORK_DIR/target.txt"
assert_command_succeeds "$ROOT_DIR/build/truncate" -s 5 "$WORK_DIR/target.txt"
truncate_size=$(wc -c < "$WORK_DIR/target.txt" | tr -d ' \r\n')
assert_text_equals "$truncate_size" '5' "truncate -s did not resize the file to the requested length"

assert_command_succeeds "$ROOT_DIR/build/truncate" -o -s 1 "$WORK_DIR/target.txt"
truncate_block_size=$(wc -c < "$WORK_DIR/target.txt" | tr -d ' \r\n')
assert_text_equals "$truncate_block_size" '512' "truncate -o should interpret the size as I/O blocks"

assert_command_succeeds "$ROOT_DIR/build/truncate" -c -s 10 "$WORK_DIR/missing.txt"
[ ! -e "$WORK_DIR/missing.txt" ] || fail "truncate -c unexpectedly created a missing file"
