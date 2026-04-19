#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir nl)

note "phase1 text: nl"

printf 'first\n\nthird\n' > "$WORK_DIR/input.txt"
"$ROOT_DIR/build/nl" -ba "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"

assert_file_contains "$WORK_DIR/out.txt" '^[[:space:]]*1[[:space:]]first$' "nl did not number the first line"
assert_file_contains "$WORK_DIR/out.txt" '^[[:space:]]*2[[:space:]]*$' "nl -ba did not number the blank line"
assert_file_contains "$WORK_DIR/out.txt" '^[[:space:]]*3[[:space:]]third$' "nl did not number the final line"
