#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/df"
note "phase1 filesystem df"

"$ROOT_DIR/build/df" > "$WORK_DIR/basic.out"
assert_file_contains "$WORK_DIR/basic.out" '^Filesystem[[:space:]]' "df header missing"

"$ROOT_DIR/build/df" -h "$WORK_DIR" > "$WORK_DIR/human.out"
assert_file_contains "$WORK_DIR/human.out" '^Filesystem[[:space:]]' "df -h header missing"
if ! grep '[0-9][0-9.]*[BKMGTP]' "$WORK_DIR/human.out" >/dev/null 2>&1; then
    fail "df -h did not produce any human-readable sizes"
fi

"$ROOT_DIR/build/df" -k "$WORK_DIR" > "$WORK_DIR/blocks.out"
assert_file_contains "$WORK_DIR/blocks.out" '1K-blocks' "df -k did not label 1K-sized output columns"

"$ROOT_DIR/build/df" -iT "$WORK_DIR" > "$WORK_DIR/inodes.out"
assert_file_contains "$WORK_DIR/inodes.out" '^Filesystem[[:space:]][[:space:]]*Type[[:space:]][[:space:]]*Inodes[[:space:]][[:space:]]*IUsed' "df -iT header missing inode/type columns"
