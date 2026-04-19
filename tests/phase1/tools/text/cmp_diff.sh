#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir cmp_diff)

note "phase1 text: cmp/diff"

printf 'same\n' > "$WORK_DIR/left.txt"
printf 'same\n' > "$WORK_DIR/right.txt"
printf 'changed\n' > "$WORK_DIR/other.txt"

assert_command_succeeds "$ROOT_DIR/build/cmp" -s "$WORK_DIR/left.txt" "$WORK_DIR/right.txt"

if "$ROOT_DIR/build/cmp" -s "$WORK_DIR/left.txt" "$WORK_DIR/other.txt"; then
    fail "cmp -s should report a difference for mismatched files"
fi

if "$ROOT_DIR/build/diff" -u "$WORK_DIR/left.txt" "$WORK_DIR/other.txt" > "$WORK_DIR/diff.out"; then
    fail "diff -u should have reported a change"
fi

assert_file_contains "$WORK_DIR/diff.out" '^--- ' "diff output was missing the left-file header"
assert_file_contains "$WORK_DIR/diff.out" '^[+][+][+] ' "diff output was missing the right-file header"
assert_file_contains "$WORK_DIR/diff.out" '^@@ ' "diff output was missing the hunk marker"
