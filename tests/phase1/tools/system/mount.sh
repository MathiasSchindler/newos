#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup mount

"$ROOT_DIR/build/mount" --help > "$WORK_DIR/mount_help.out"
assert_file_contains "$WORK_DIR/mount_help.out" '^Usage: .*mount' "mount --help did not print usage"
assert_file_contains "$WORK_DIR/mount_help.out" 'Use -p or --mkdir' "mount --help did not mention --mkdir"

"$ROOT_DIR/build/umount" --help > "$WORK_DIR/umount_help.out"
assert_file_contains "$WORK_DIR/umount_help.out" '^Usage: .*umount' "umount --help did not print usage"
assert_file_contains "$WORK_DIR/umount_help.out" 'mounted source' "umount --help did not explain source-based unmounts"

if [ -r /proc/self/mounts ] || [ -r /proc/mounts ]; then
    "$ROOT_DIR/build/mount" > "$WORK_DIR/mount_list.out"
    assert_file_contains "$WORK_DIR/mount_list.out" ' on / type ' "mount did not format the current mount table"
    "$ROOT_DIR/build/mount" / > "$WORK_DIR/mount_root.out"
    assert_file_contains "$WORK_DIR/mount_root.out" ' on / type ' "mount PATH did not filter the current mount table"
fi

if "$ROOT_DIR/build/umount" >/dev/null 2>&1; then
    fail "umount without a target should fail"
fi

if "$ROOT_DIR/build/mount" -t tmpfs --mkdir tmpfs "$WORK_DIR/auto_mount_target" >/dev/null 2>&1; then
    "$ROOT_DIR/build/umount" "$WORK_DIR/auto_mount_target" >/dev/null 2>&1 || true
fi
if [ ! -d "$WORK_DIR/auto_mount_target" ]; then
    fail "mount --mkdir did not create the requested target directory"
fi
