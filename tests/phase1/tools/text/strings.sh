#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir strings)

note "phase1 text: strings"

printf '\001\002HELLO123\000bye\000' > "$WORK_DIR/input.bin"
"$ROOT_DIR/build/strings" "$WORK_DIR/input.bin" > "$WORK_DIR/out.txt"

assert_file_contains "$WORK_DIR/out.txt" '^HELLO123$' "strings did not extract the printable payload"
if grep '^bye$' "$WORK_DIR/out.txt" >/dev/null 2>&1; then
    fail "strings should not emit short sequences below the minimum length"
fi
