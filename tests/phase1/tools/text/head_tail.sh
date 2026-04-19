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

printf 'abcdef12345' > "$WORK_DIR/bytes.txt"
head_bytes=$("$ROOT_DIR/build/head" -c 5 "$WORK_DIR/bytes.txt" | tr -d '\r\n')
assert_text_equals "$head_bytes" 'abcde' "head -c returned the wrong leading bytes"
tail_bytes=$("$ROOT_DIR/build/tail" -c 5 "$WORK_DIR/bytes.txt" | tr -d '\r\n')
assert_text_equals "$tail_bytes" '12345' "tail -c returned the wrong trailing bytes"

"$ROOT_DIR/build/head" -n +3 "$WORK_DIR/input.txt" > "$WORK_DIR/head_plus.out"
printf 'gamma\n' > "$WORK_DIR/head_plus.expected"
assert_files_equal "$WORK_DIR/head_plus.expected" "$WORK_DIR/head_plus.out" "head -n +N did not start at the requested line"

"$ROOT_DIR/build/tail" -v "$WORK_DIR/input.txt" > "$WORK_DIR/tail_v.out"
assert_file_contains "$WORK_DIR/tail_v.out" '^==> .*input.txt <==$' "tail -v should force a file header"
