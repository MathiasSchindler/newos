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

printf 'alpha\n\n\nbeta\n' > "$WORK_DIR/flags.txt"
"$ROOT_DIR/build/cat" -b "$WORK_DIR/flags.txt" > "$WORK_DIR/cat_b.out"
assert_file_contains "$WORK_DIR/cat_b.out" '^[[:space:]]*2[[:space:]]beta$' "cat -b did not skip blank lines when numbering"
"$ROOT_DIR/build/cat" -s "$WORK_DIR/flags.txt" > "$WORK_DIR/cat_s.out"
printf 'alpha\n\nbeta\n' > "$WORK_DIR/cat_s.expected"
assert_files_equal "$WORK_DIR/cat_s.expected" "$WORK_DIR/cat_s.out" "cat -s did not squeeze repeated blank lines"
printf 'A\tB\001\n' > "$WORK_DIR/cat_visible.txt"
"$ROOT_DIR/build/cat" -A "$WORK_DIR/cat_visible.txt" > "$WORK_DIR/cat_visible.out"
assert_file_contains "$WORK_DIR/cat_visible.out" '^A\^IB\^A\$$' "cat -A did not render control characters visibly"

ODD_FILE="$WORK_DIR/odd name [v1] !.txt"
printf 'odd-data\n' > "$ODD_FILE"
"$ROOT_DIR/build/cat" "$ODD_FILE" > "$WORK_DIR/odd.out"
assert_file_contains "$WORK_DIR/odd.out" '^odd-data$' "cat did not read a safely quoted odd filename"
