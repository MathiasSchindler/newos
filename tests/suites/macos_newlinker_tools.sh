#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

BUILD_DIR=${NEWOS_MACOS_NEWLINKER_BUILD_DIR:-$ROOT_DIR/build/newlinker-macos-aarch64}
WORK_DIR="$ROOT_DIR/tests/tmp/macos_newlinker_tools"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

note "macos newlinker tools"

assert_command_succeeds "$BUILD_DIR/true"
if "$BUILD_DIR/false"; then
    fail "false returned success"
fi

"$BUILD_DIR/echo" hello world > "$WORK_DIR/echo.out"
assert_file_contains "$WORK_DIR/echo.out" '^hello world$' "echo output mismatch"

"$BUILD_DIR/printf" 'hi %d' 7 > "$WORK_DIR/printf.out"
assert_file_contains "$WORK_DIR/printf.out" '^hi 7$' "printf output mismatch"

"$BUILD_DIR/rev" > "$WORK_DIR/rev.out" <<'EOF'
abc
EOF
assert_file_contains "$WORK_DIR/rev.out" '^cba$' "rev output mismatch"

"$BUILD_DIR/seq" 1 3 > "$WORK_DIR/seq.out"
assert_file_contains "$WORK_DIR/seq.out" '^1$' "seq output missing 1"
assert_file_contains "$WORK_DIR/seq.out" '^3$' "seq output missing 3"

printf 'one\ntwo\n' > "$WORK_DIR/input.txt"
"$BUILD_DIR/cat" "$WORK_DIR/input.txt" > "$WORK_DIR/cat.out"
assert_files_equal "$WORK_DIR/input.txt" "$WORK_DIR/cat.out" "cat output mismatch"

"$BUILD_DIR/basename" /tmp/file.txt > "$WORK_DIR/basename.out"
assert_file_contains "$WORK_DIR/basename.out" '^file.txt$' "basename output mismatch"

"$BUILD_DIR/dirname" /tmp/a/file.txt > "$WORK_DIR/dirname.out"
assert_file_contains "$WORK_DIR/dirname.out" '^/tmp/a$' "dirname output mismatch"

"$BUILD_DIR/cut" -c 2 "$WORK_DIR/input.txt" > "$WORK_DIR/cut.out"
assert_file_contains "$WORK_DIR/cut.out" '^n$' "cut output missing n"
assert_file_contains "$WORK_DIR/cut.out" '^w$' "cut output missing w"

printf 'abc' | "$BUILD_DIR/tr" a-z A-Z > "$WORK_DIR/tr.out"
assert_file_contains "$WORK_DIR/tr.out" '^ABC$' "tr output mismatch"

"$BUILD_DIR/wc" -l "$WORK_DIR/input.txt" > "$WORK_DIR/wc.out"
assert_file_contains "$WORK_DIR/wc.out" ' 2 ' "wc line count mismatch"

if command -v otool >/dev/null 2>&1; then
    otool -L "$BUILD_DIR/rev" > "$WORK_DIR/rev.otool"
    if grep -q '\.dylib' "$WORK_DIR/rev.otool"; then
        fail "project-linked Mach-O tool should not import dylibs yet"
    fi
fi

echo "MACOS_NEWLINKER_TOOLS_OK"