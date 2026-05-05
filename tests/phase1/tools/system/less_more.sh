#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup less_more

printf 'alpha\nbeta\nGamma\n' > "$WORK_DIR/pager.txt"

"$ROOT_DIR/build/less" -N -p gamma "$WORK_DIR/pager.txt" > "$WORK_DIR/less.out"
assert_file_contains "$WORK_DIR/less.out" '^3[[:space:]][[:space:]]*Gamma$' "less -p did not jump to the requested match"
if grep -q '^1[[:space:]][[:space:]]*alpha$' "$WORK_DIR/less.out"; then
    fail "less -p printed lines before the requested match"
fi

"$ROOT_DIR/build/less" -N +2 "$WORK_DIR/pager.txt" > "$WORK_DIR/less_line_jump.out"
assert_file_contains "$WORK_DIR/less_line_jump.out" '^2[[:space:]][[:space:]]*beta$' "less +NUMBER did not jump to the requested line"
if grep -q '^1[[:space:]][[:space:]]*alpha$' "$WORK_DIR/less_line_jump.out"; then
    fail "less +NUMBER printed lines before the requested line"
fi

"$ROOT_DIR/build/less" -N +50% "$WORK_DIR/pager.txt" > "$WORK_DIR/less_percent_jump.out"
assert_file_contains "$WORK_DIR/less_percent_jump.out" '^2[[:space:]][[:space:]]*beta$' "less +PERCENT% did not jump near the requested percentage"

seq 1 12000 > "$WORK_DIR/large.txt"
"$ROOT_DIR/build/less" "$WORK_DIR/large.txt" > "$WORK_DIR/less_large.out"
assert_file_contains "$WORK_DIR/less_large.out" '^12000$' "less did not stream large noninteractive input to the end"

"$ROOT_DIR/build/more" -N +/beta "$WORK_DIR/pager.txt" > "$WORK_DIR/more.out"
assert_file_contains "$WORK_DIR/more.out" '^2[[:space:]][[:space:]]*beta$' "more +/pattern did not start at the requested match"
assert_file_contains "$WORK_DIR/more.out" '^3[[:space:]][[:space:]]*Gamma$' "more +/pattern did not keep following content"
