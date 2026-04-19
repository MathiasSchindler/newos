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

"$ROOT_DIR/build/strings" -t x "$WORK_DIR/input.bin" > "$WORK_DIR/offset.out"
assert_file_contains "$WORK_DIR/offset.out" '^[0-9a-f][0-9a-f]* HELLO123$' "strings -t x did not include the expected offset"

printf 'W\000i\000d\000e\000\000\000' > "$WORK_DIR/wide_le.bin"
"$ROOT_DIR/build/strings" -e l -n 4 "$WORK_DIR/wide_le.bin" > "$WORK_DIR/wide_le.out"
assert_file_contains "$WORK_DIR/wide_le.out" '^Wide$' "strings -e l did not decode UTF-16LE-style text"
