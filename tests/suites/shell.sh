#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

WORK_DIR="$ROOT_DIR/tests/tmp/shell"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

note "shell"

printf 'export FOO=bar\necho $FOO\nfalse\necho $?\necho ${FOO}\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/sh.out"
assert_file_contains "$WORK_DIR/sh.out" '^bar$' "shell variable expansion failed"
assert_file_contains "$WORK_DIR/sh.out" '^1$' "shell status expansion failed"

printf 'echo first\necho second\nhistory\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/history.out"
assert_file_contains "$WORK_DIR/history.out" '1  echo first' "shell history missing first command"
assert_file_contains "$WORK_DIR/history.out" '2  echo second' "shell history missing second command"

printf 'command -v ls\nalias hi="echo alias-ok"\nhi\nfn() { echo func-ok; }\nfn\ncat <<EOF\nheredoc-line\nEOF\n' | "$ROOT_DIR/build/sh" > "$WORK_DIR/features.out"
assert_file_contains "$WORK_DIR/features.out" '/ls$' "shell command -v failed"
assert_file_contains "$WORK_DIR/features.out" '^alias-ok$' "shell alias failed"
assert_file_contains "$WORK_DIR/features.out" '^func-ok$' "shell function failed"
assert_file_contains "$WORK_DIR/features.out" '^heredoc-line$' "shell here-document failed"
