#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/archive"
note "phase1 filesystem archive"

mkdir -p "$WORK_DIR/tar_src/sub"
printf 'root-data\n' > "$WORK_DIR/tar_src/root.txt"
printf 'nested-data\n' > "$WORK_DIR/tar_src/sub/nested.txt"

(
    cd "$WORK_DIR"
    assert_command_succeeds "$ROOT_DIR/build/tar" -cf sample.tar tar_src
    assert_command_succeeds "$ROOT_DIR/build/tar" -tf sample.tar > tar_list.out
)
assert_file_contains "$WORK_DIR/tar_list.out" '^tar_src/root.txt$' "tar -t did not list the archive root file"
assert_file_contains "$WORK_DIR/tar_list.out" '^tar_src/sub/nested.txt$' "tar -t did not list the nested archive file"

mkdir -p "$WORK_DIR/tar_extract"
(
    cd "$WORK_DIR/tar_extract"
    assert_command_succeeds "$ROOT_DIR/build/tar" -xf "$WORK_DIR/sample.tar"
)
assert_file_contains "$WORK_DIR/tar_extract/tar_src/root.txt" '^root-data$' "tar -x did not restore the root file"
assert_file_contains "$WORK_DIR/tar_extract/tar_src/sub/nested.txt" '^nested-data$' "tar -x did not restore the nested file"

printf 'one\n' > "$WORK_DIR/a.txt"
printf 'two\n' > "$WORK_DIR/b.txt"
assert_command_succeeds "$ROOT_DIR/build/ar" rc "$WORK_DIR/test.a" "$WORK_DIR/a.txt" "$WORK_DIR/b.txt"
assert_command_succeeds "$ROOT_DIR/build/ar" t "$WORK_DIR/test.a" > "$WORK_DIR/ar_list.out"
assert_file_contains "$WORK_DIR/ar_list.out" '^a.txt$' "ar t did not list the first archived file"
assert_file_contains "$WORK_DIR/ar_list.out" '^b.txt$' "ar t did not list the second archived file"

mkdir -p "$WORK_DIR/ar_extract"
(
    cd "$WORK_DIR/ar_extract"
    assert_command_succeeds "$ROOT_DIR/build/ar" x "$WORK_DIR/test.a"
)
assert_file_contains "$WORK_DIR/ar_extract/a.txt" '^one$' "ar x did not restore the first file"
assert_file_contains "$WORK_DIR/ar_extract/b.txt" '^two$' "ar x did not restore the second file"
