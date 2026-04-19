#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir wc)

note "phase1 text: wc"

printf 'one two\nthree\n' > "$WORK_DIR/input.txt"

set -- $("$ROOT_DIR/build/wc" -lwc "$WORK_DIR/input.txt")
assert_text_equals "$1" '2' "wc -l reported the wrong number of lines"
assert_text_equals "$2" '3' "wc -w reported the wrong number of words"
assert_text_equals "$3" '14' "wc -c reported the wrong number of bytes"
