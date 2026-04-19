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

cmp_list_out=$("$ROOT_DIR/build/cmp" -l "$WORK_DIR/left.txt" "$WORK_DIR/other.txt" | tr -d '\r')
printf '%s\n' "$cmp_list_out" > "$WORK_DIR/cmp_l.out"
assert_file_contains "$WORK_DIR/cmp_l.out" '^[0-9][0-9]* ' "cmp -l did not report the differing byte positions"

if "$ROOT_DIR/build/diff" -c "$WORK_DIR/left.txt" "$WORK_DIR/other.txt" > "$WORK_DIR/diff_c.out"; then
    fail "diff -c should have reported a change"
fi
assert_file_contains "$WORK_DIR/diff_c.out" '^\*\*\* ' "diff -c output was missing the left header"
assert_file_contains "$WORK_DIR/diff_c.out" '^--- ' "diff -c output was missing the right header"
