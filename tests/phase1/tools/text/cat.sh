#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir cat)

note "phase1 text: cat"

printf 'alpha\n' > "$WORK_DIR/left.txt"
printf 'beta\ngamma\n' > "$WORK_DIR/right.txt"

"$ROOT_DIR/build/cat" -n "$WORK_DIR/left.txt" "$WORK_DIR/right.txt" > "$WORK_DIR/out.txt"

assert_file_contains "$WORK_DIR/out.txt" '^[[:space:]]*1[[:space:]]alpha$' "cat -n did not number the first line"
assert_file_contains "$WORK_DIR/out.txt" '^[[:space:]]*2[[:space:]]beta$' "cat did not concatenate the second line correctly"
assert_file_contains "$WORK_DIR/out.txt" '^[[:space:]]*3[[:space:]]gamma$' "cat did not continue numbering across files"
