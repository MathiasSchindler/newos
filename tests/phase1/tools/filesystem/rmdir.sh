#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/rmdir"
note "phase1 filesystem rmdir"

mkdir -p "$WORK_DIR/tree/a/b"
(
    cd "$WORK_DIR/tree" &&
    "$ROOT_DIR/build/rmdir" -pv a/b
) > "$WORK_DIR/remove.out"
assert_file_contains "$WORK_DIR/remove.out" 'removed directory ' "rmdir -pv did not report removals"
[ ! -d "$WORK_DIR/tree/a/b" ] || fail "rmdir -p left the leaf directory behind"
[ ! -d "$WORK_DIR/tree/a" ] || fail "rmdir -p left the parent directory behind"

mkdir -p "$WORK_DIR/ignore/parent/child"
printf 'stay\n' > "$WORK_DIR/ignore/parent/keep.txt"
assert_command_succeeds "$ROOT_DIR/build/rmdir" -p --ignore-fail-on-non-empty "$WORK_DIR/ignore/parent/child"
[ ! -d "$WORK_DIR/ignore/parent/child" ] || fail "rmdir --ignore-fail-on-non-empty did not remove the requested leaf"
[ -d "$WORK_DIR/ignore/parent" ] || fail "rmdir --ignore-fail-on-non-empty removed a non-empty parent"
