#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/mkdir"
note "phase1 filesystem mkdir"

"$ROOT_DIR/build/mkdir" -vp "$WORK_DIR/nested/a/b" > "$WORK_DIR/verbose.out"
[ -d "$WORK_DIR/nested/a/b" ] || fail "mkdir -p did not create the nested directory tree"
assert_file_contains "$WORK_DIR/verbose.out" 'created directory ' "mkdir -v did not report created directories"

assert_command_succeeds "$ROOT_DIR/build/mkdir" -m u=rwx,go= "$WORK_DIR/private"
"$ROOT_DIR/build/stat" "$WORK_DIR/private" > "$WORK_DIR/private.stat"
assert_file_contains "$WORK_DIR/private.stat" 'Mode: drwx------' "mkdir -m did not apply the requested mode"
