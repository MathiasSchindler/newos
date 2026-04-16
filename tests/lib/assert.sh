#!/bin/sh

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

assert_file_contains() {
    file="$1"
    pattern="$2"
    description="$3"
    grep -q "$pattern" "$file" || fail "$description"
}

assert_text_equals() {
    actual="$1"
    expected="$2"
    description="$3"
    [ "$actual" = "$expected" ] || fail "$description: expected [$expected], got [$actual]"
}

assert_command_succeeds() {
    "$@" || fail "command failed: $*"
}

assert_files_equal() {
    left="$1"
    right="$2"
    description="$3"
    cmp -s "$left" "$right" || fail "$description"
}

note() {
    echo "==> $1"
}
