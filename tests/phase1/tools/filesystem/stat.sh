#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/stat"
note "phase1 filesystem stat"

printf 'data\n' > "$WORK_DIR/data.txt"
ln -sf data.txt "$WORK_DIR/data.link"

"$ROOT_DIR/build/stat" "$WORK_DIR/data.txt" > "$WORK_DIR/basic.out"
assert_file_contains "$WORK_DIR/basic.out" '^Size:' "stat output missing size"
assert_file_contains "$WORK_DIR/basic.out" 'Type: file' "stat output missing file type"
assert_file_contains "$WORK_DIR/basic.out" '^Access:' "stat output missing access time"
assert_file_contains "$WORK_DIR/basic.out" '^Change:' "stat output missing change time"

stat_format_out=$("$ROOT_DIR/build/stat" -c '%F %a %n' "$WORK_DIR/data.txt" | tr -d '\r')
printf '%s\n' "$stat_format_out" > "$WORK_DIR/format.out"
assert_file_contains "$WORK_DIR/format.out" '^file [0-7][0-7][0-7][0-7]*[[:space:]].*data\.txt$' "stat -c format output was incomplete"

stat_follow_out=$("$ROOT_DIR/build/stat" -L -c '%F' "$WORK_DIR/data.link" | tr -d '\r\n')
assert_text_equals "$stat_follow_out" 'file' "stat -L did not follow the symlink"

"$ROOT_DIR/build/stat" -f "$WORK_DIR" > "$WORK_DIR/filesystem.out"
assert_file_contains "$WORK_DIR/filesystem.out" '^Filesystem:' "stat -f did not print filesystem information"
