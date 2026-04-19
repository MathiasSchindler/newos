#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir cut)

note "phase1 text: cut"

printf 'left:middle:right\n' > "$WORK_DIR/fields.txt"
"$ROOT_DIR/build/cut" -d : -f 2 "$WORK_DIR/fields.txt" > "$WORK_DIR/field.out"
assert_file_contains "$WORK_DIR/field.out" '^middle$' "cut -d/-f selected the wrong field"

printf 'ABCDE\n' > "$WORK_DIR/chars.txt"
"$ROOT_DIR/build/cut" -c 2-4 "$WORK_DIR/chars.txt" > "$WORK_DIR/chars.out"
assert_file_contains "$WORK_DIR/chars.out" '^BCD$' "cut -c selected the wrong character range"
