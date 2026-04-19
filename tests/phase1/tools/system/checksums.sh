#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup checksums

printf 'alpha\n' > "$WORK_DIR/sample.txt"

assert_command_succeeds "$ROOT_DIR/build/md5sum" "$WORK_DIR/sample.txt" > "$WORK_DIR/md5.out"
assert_file_contains "$WORK_DIR/md5.out" '^[0-9a-f]\{32\}  .*/sample.txt$' "md5sum did not print a 32-hex digest"
assert_command_succeeds "$ROOT_DIR/build/md5sum" -c "$WORK_DIR/md5.out" > "$WORK_DIR/md5_check.out"
assert_file_contains "$WORK_DIR/md5_check.out" 'OK$' "md5sum -c did not verify the checksum file"

assert_command_succeeds "$ROOT_DIR/build/sha256sum" "$WORK_DIR/sample.txt" > "$WORK_DIR/sha256.out"
assert_file_contains "$WORK_DIR/sha256.out" '^[0-9a-f]\{64\}  .*/sample.txt$' "sha256sum did not print a 64-hex digest"
assert_command_succeeds "$ROOT_DIR/build/sha256sum" -c "$WORK_DIR/sha256.out" > "$WORK_DIR/sha256_check.out"
assert_file_contains "$WORK_DIR/sha256_check.out" 'OK$' "sha256sum -c did not verify the checksum file"

assert_command_succeeds "$ROOT_DIR/build/sha512sum" "$WORK_DIR/sample.txt" > "$WORK_DIR/sha512.out"
assert_file_contains "$WORK_DIR/sha512.out" '^[0-9a-f]\{128\}  .*/sample.txt$' "sha512sum did not print a 128-hex digest"
assert_command_succeeds "$ROOT_DIR/build/sha512sum" -c "$WORK_DIR/sha512.out" > "$WORK_DIR/sha512_check.out"
assert_file_contains "$WORK_DIR/sha512_check.out" 'OK$' "sha512sum -c did not verify the checksum file"
