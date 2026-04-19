#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/dirname"
note "phase1 filesystem dirname"

mkdir -p "$WORK_DIR/sub/inner"
actual_dirname=$("$ROOT_DIR/build/dirname" "$WORK_DIR/sub/inner/example.txt" | tr -d '\r\n')
assert_text_equals "$actual_dirname" "$WORK_DIR/sub/inner" "dirname returned the wrong parent path"

dirname_multi=$("$ROOT_DIR/build/dirname" "$WORK_DIR/sub/inner/example.txt" "$WORK_DIR/root.txt" | tr -d '\r')
printf '%s\n' "$dirname_multi" > "$WORK_DIR/multi.out"
assert_file_contains "$WORK_DIR/multi.out" '^.*/sub/inner$' "dirname multi-path mode missed the nested directory"
assert_file_contains "$WORK_DIR/multi.out" "^$WORK_DIR\$" "dirname multi-path mode missed the work directory itself"

"$ROOT_DIR/build/dirname" --zero "$WORK_DIR/sub/inner/example.txt" "$WORK_DIR/root.txt" | tr '\0' '\n' > "$WORK_DIR/zero.out"
assert_file_contains "$WORK_DIR/zero.out" '^.*/sub/inner$' "dirname --zero did not preserve the nested directory path"
