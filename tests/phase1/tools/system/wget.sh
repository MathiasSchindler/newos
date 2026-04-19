#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup wget

printf 'wget sample\n' > "$WORK_DIR/source.txt"
assert_command_succeeds "$ROOT_DIR/build/wget" -q -O "$WORK_DIR/copy.txt" "file://$WORK_DIR/source.txt"
assert_files_equal "$WORK_DIR/source.txt" "$WORK_DIR/copy.txt" "wget file:// download failed"

wget_stdout=$("$ROOT_DIR/build/wget" -q -O - "file://$WORK_DIR/source.txt" | tr -d '\r\n')
assert_text_equals "$wget_stdout" 'wget sample' "wget -O - did not stream the fetched content"
