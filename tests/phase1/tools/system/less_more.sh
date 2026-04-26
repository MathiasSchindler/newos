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

"$ROOT_DIR/build/more" -N +/beta "$WORK_DIR/pager.txt" > "$WORK_DIR/more.out"
assert_file_contains "$WORK_DIR/more.out" '^2[[:space:]][[:space:]]*beta$' "more +/pattern did not start at the requested match"
assert_file_contains "$WORK_DIR/more.out" '^3[[:space:]][[:space:]]*Gamma$' "more +/pattern did not keep following content"