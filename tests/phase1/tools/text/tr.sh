#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir tr)

note "phase1 text: tr"

printf 'abba cab\n' > "$WORK_DIR/input.txt"
"$ROOT_DIR/build/tr" 'ab' 'AB' < "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"
printf 'ABBA cAB\n' > "$WORK_DIR/expected.txt"
assert_files_equal "$WORK_DIR/expected.txt" "$WORK_DIR/out.txt" "tr did not translate characters one-for-one"
