#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup man

"$ROOT_DIR/build/man" env > "$WORK_DIR/man_env.out"
assert_file_contains "$WORK_DIR/man_env.out" '^ENV$' "man did not open the env manual page"
assert_file_contains "$WORK_DIR/man_env.out" 'emit NUL-delimited output with -0' "man output missed env option details"

"$ROOT_DIR/build/man" -k compiler > "$WORK_DIR/man_search.out"
assert_file_contains "$WORK_DIR/man_search.out" '^ncc (1)$' "man -k did not find the compiler page"
