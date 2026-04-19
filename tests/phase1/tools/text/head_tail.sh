#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir head_tail)

note "phase1 text: head/tail"

printf 'alpha\nbeta\ngamma\n' > "$WORK_DIR/input.txt"

"$ROOT_DIR/build/head" -n 2 "$WORK_DIR/input.txt" > "$WORK_DIR/head.out"
printf 'alpha\nbeta\n' > "$WORK_DIR/head.expected"
assert_files_equal "$WORK_DIR/head.expected" "$WORK_DIR/head.out" "head -n returned the wrong leading lines"

"$ROOT_DIR/build/tail" -n 2 "$WORK_DIR/input.txt" > "$WORK_DIR/tail.out"
printf 'beta\ngamma\n' > "$WORK_DIR/tail.expected"
assert_files_equal "$WORK_DIR/tail.expected" "$WORK_DIR/tail.out" "tail -n returned the wrong trailing lines"
