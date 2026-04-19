#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup make

printf 'MSG ?= fallback\nall:\n\tprintf '\''%%s\\n'\'' $(MSG) > out.txt\n' > "$WORK_DIR/Makefile"

(
    cd "$WORK_DIR"
    assert_command_succeeds "$ROOT_DIR/build/make" MSG=phase1
)
assert_file_contains "$WORK_DIR/out.txt" '^phase1$' "make did not apply a command-line variable override"

(
    cd "$WORK_DIR"
    assert_command_succeeds "$ROOT_DIR/build/make" -n MSG=dry-run > dry.out
)
assert_file_contains "$WORK_DIR/dry.out" "printf '%s\\\\n' dry-run > out.txt" "make -n did not print the recipe without executing it"
