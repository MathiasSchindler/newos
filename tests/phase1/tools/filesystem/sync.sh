#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/sync"
note "phase1 filesystem sync"

printf 'sync\n' > "$WORK_DIR/target.txt"
"${TEST_BIN_DIR}/sync" -d -v "$WORK_DIR/target.txt" > "$WORK_DIR/out"
assert_file_contains "$WORK_DIR/out" 'synced .*target.txt' "sync -v did not report the synced path"

"${TEST_BIN_DIR}/sync" -v > "$WORK_DIR/out_all"
assert_file_contains "$WORK_DIR/out_all" 'synced all filesystems' "sync -v without paths did not confirm the global sync"

assert_command_succeeds "${TEST_BIN_DIR}/sync" -d
