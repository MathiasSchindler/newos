#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup mount

"$ROOT_DIR/build/mount" --help > "$WORK_DIR/mount_help.out"
assert_file_contains "$WORK_DIR/mount_help.out" '^Usage: .*mount' "mount --help did not print usage"

"$ROOT_DIR/build/umount" --help > "$WORK_DIR/umount_help.out"
assert_file_contains "$WORK_DIR/umount_help.out" '^Usage: .*umount' "umount --help did not print usage"

if [ -r /proc/self/mounts ] || [ -r /proc/mounts ]; then
    "$ROOT_DIR/build/mount" > "$WORK_DIR/mount_list.out"
    assert_file_contains "$WORK_DIR/mount_list.out" '^[^ ][^ ]* [^ ][^ ]* [^ ][^ ]*' "mount did not list the current mount table"
fi

if "$ROOT_DIR/build/umount" >/dev/null 2>&1; then
    fail "umount without a target should fail"
fi
