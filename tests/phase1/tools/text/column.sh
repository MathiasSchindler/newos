#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir column)

note "phase1 text: column"

printf 'a 1\nlong 22\n' > "$WORK_DIR/input.txt"
"$ROOT_DIR/build/column" -t "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"
printf 'a     1\nlong  22\n' > "$WORK_DIR/expected.txt"
assert_files_equal "$WORK_DIR/expected.txt" "$WORK_DIR/out.txt" "column -t did not align the table cells cleanly"
