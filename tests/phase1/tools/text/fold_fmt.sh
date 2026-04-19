#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir fold_fmt)

note "phase1 text: fold/fmt"

printf 'abcd1234\n' > "$WORK_DIR/fold.txt"
"$ROOT_DIR/build/fold" -w 4 "$WORK_DIR/fold.txt" > "$WORK_DIR/fold.out"
printf 'abcd\n1234\n' > "$WORK_DIR/fold.expected"
assert_files_equal "$WORK_DIR/fold.expected" "$WORK_DIR/fold.out" "fold did not wrap at the requested width"

printf 'foo bar baz\n' > "$WORK_DIR/fmt.txt"
"$ROOT_DIR/build/fmt" -w 6 "$WORK_DIR/fmt.txt" > "$WORK_DIR/fmt.out"
printf 'foo\nbar\nbaz\n' > "$WORK_DIR/fmt.expected"
assert_files_equal "$WORK_DIR/fmt.expected" "$WORK_DIR/fmt.out" "fmt did not reflow the line into the requested width"
