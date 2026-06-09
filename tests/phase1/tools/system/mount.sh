#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup mount

"${TEST_BIN_DIR}/mount" --help > "$WORK_DIR/mount_help.out"
assert_file_contains "$WORK_DIR/mount_help.out" '^Usage: .*mount' "mount --help did not print usage"
assert_file_contains "$WORK_DIR/mount_help.out" 'Use -p or --mkdir' "mount --help did not mention --mkdir"

"${TEST_BIN_DIR}/umount" --help > "$WORK_DIR/umount_help.out"
assert_file_contains "$WORK_DIR/umount_help.out" '^Usage: .*umount' "umount --help did not print usage"
assert_file_contains "$WORK_DIR/umount_help.out" 'mounted source' "umount --help did not explain source-based unmounts"

if [ -r /proc/self/mounts ] || [ -r /proc/mounts ]; then
    "${TEST_BIN_DIR}/mount" > "$WORK_DIR/mount_list.out"
    assert_file_contains "$WORK_DIR/mount_list.out" ' on / type ' "mount did not format the current mount table"
    "${TEST_BIN_DIR}/mount" / > "$WORK_DIR/mount_root.out"
    assert_file_contains "$WORK_DIR/mount_root.out" ' on / type ' "mount PATH did not filter the current mount table"
fi

if "${TEST_BIN_DIR}/umount" >/dev/null 2>&1; then
    fail "umount without a target should fail"
fi

if "${TEST_BIN_DIR}/mount" -t tmpfs --mkdir tmpfs "$WORK_DIR/auto_mount_target" >/dev/null 2>&1; then
    "${TEST_BIN_DIR}/umount" "$WORK_DIR/auto_mount_target" >/dev/null 2>&1 || true
fi
if [ ! -d "$WORK_DIR/auto_mount_target" ]; then
    fail "mount --mkdir did not create the requested target directory"
fi
