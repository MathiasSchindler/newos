#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/basename"
note "phase1 filesystem basename"

mkdir -p "$WORK_DIR/path"
actual_basename=$("$ROOT_DIR/build/basename" "$WORK_DIR/path/example.txt" .txt | tr -d '\r\n')
assert_text_equals "$actual_basename" 'example' "basename did not strip the suffix from a single path"

printf 'a\n' > "$WORK_DIR/alpha.txt"
printf 'b\n' > "$WORK_DIR/beta.txt"
basename_multi=$("$ROOT_DIR/build/basename" -a -s .txt "$WORK_DIR/alpha.txt" "$WORK_DIR/beta.txt" | tr -d '\r')
printf '%s\n' "$basename_multi" > "$WORK_DIR/multi.out"
assert_file_contains "$WORK_DIR/multi.out" '^alpha$' "basename -a/-s missed the first path"
assert_file_contains "$WORK_DIR/multi.out" '^beta$' "basename -a/-s missed the second path"

"$ROOT_DIR/build/basename" --multiple --suffix=.txt --zero "$WORK_DIR/alpha.txt" "$WORK_DIR/beta.txt" | tr '\0' '\n' > "$WORK_DIR/zero.out"
assert_file_contains "$WORK_DIR/zero.out" '^alpha$' "basename --zero did not include the first path"
assert_file_contains "$WORK_DIR/zero.out" '^beta$' "basename --zero did not include the second path"
