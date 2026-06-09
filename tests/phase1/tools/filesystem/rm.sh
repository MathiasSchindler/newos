#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/rm"
note "phase1 filesystem rm"

printf 'keep\n' > "$WORK_DIR/prompt.txt"
printf 'n\n' | "${TEST_BIN_DIR}/rm" -I "$WORK_DIR/prompt.txt" >/dev/null 2>&1
[ -f "$WORK_DIR/prompt.txt" ] || fail "rm -I removed a file after a negative confirmation"

printf 'drop\n' > "$WORK_DIR/verbose.txt"
"${TEST_BIN_DIR}/rm" -v "$WORK_DIR/verbose.txt" > "$WORK_DIR/verbose.out"
assert_file_contains "$WORK_DIR/verbose.out" 'removed ' "rm -v did not report the removal"
[ ! -e "$WORK_DIR/verbose.txt" ] || fail "rm -v left the file behind"

mkdir -p "$WORK_DIR/protected"
printf 'still-here\n' > "$WORK_DIR/protected/file.txt"
if (
    cd "$WORK_DIR/protected" &&
    "${TEST_BIN_DIR}/rm" -rf . >/dev/null 2>&1
); then
    fail "rm should refuse to remove ."
fi
[ -f "$WORK_DIR/protected/file.txt" ] || fail "rm removed files even though . should be protected"
