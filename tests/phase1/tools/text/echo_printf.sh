#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir echo_printf)

note "phase1 text: echo/printf"

echo_out=$("$ROOT_DIR/build/echo" -n 'phase1 ready')
assert_text_equals "$echo_out" 'phase1 ready' "echo -n did not preserve the text exactly"

"$ROOT_DIR/build/printf" 'value:%04d:%s\n' 7 ok > "$WORK_DIR/printf.out"
printf 'value:0007:ok\n' > "$WORK_DIR/printf.expected"
assert_files_equal "$WORK_DIR/printf.expected" "$WORK_DIR/printf.out" "printf formatting output changed unexpectedly"
