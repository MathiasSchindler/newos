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

mkdir -p "$WORK_DIR/tar_filter_src/keep/sub" "$WORK_DIR/tar_filter_src/skip"
long_rel='tar_filter_src/keep/this-is-a-very-long-directory-name-0123456789abcdef/another-very-long-segment-0123456789abcdef/long.txt'
mkdir -p "$(dirname "$WORK_DIR/$long_rel")"
printf 'keep-data\n' > "$WORK_DIR/tar_filter_src/keep/sub/keep.txt"
printf 'skip-data\n' > "$WORK_DIR/tar_filter_src/skip/omit.txt"
printf 'long-path-data\n' > "$WORK_DIR/$long_rel"
(
    cd "$WORK_DIR"
    assert_command_succeeds "$ROOT_DIR/build/tar" --exclude='tar_filter_src/skip/*' -cf filtered.tar tar_filter_src
    assert_command_succeeds "$ROOT_DIR/build/tar" -tf filtered.tar 'tar_filter_src/keep/*' > filtered_list.out
)
assert_file_contains "$WORK_DIR/filtered_list.out" '^tar_filter_src/keep/sub/keep.txt$' "tar member filtering did not keep the requested file"
assert_file_contains "$WORK_DIR/filtered_list.out" 'another-very-long-segment-0123456789abcdef/long.txt$' "tar did not preserve a long ustar path"
if grep -q 'tar_filter_src/skip/omit.txt' "$WORK_DIR/filtered_list.out"; then
    fail "tar --exclude did not omit the excluded file"
fi

mkdir -p "$WORK_DIR/tar_stripped"
(
    cd "$WORK_DIR/tar_stripped"
    assert_command_succeeds "$ROOT_DIR/build/tar" --strip-components=1 -xf "$WORK_DIR/filtered.tar" 'tar_filter_src/keep/*'
)
assert_file_contains "$WORK_DIR/tar_stripped/keep/sub/keep.txt" '^keep-data$' "tar --strip-components did not restore the filtered file"
assert_file_contains "$WORK_DIR/tar_stripped/keep/this-is-a-very-long-directory-name-0123456789abcdef/another-very-long-segment-0123456789abcdef/long.txt" '^long-path-data$' "tar --strip-components did not restore the long-path file"
if [ -e "$WORK_DIR/tar_stripped/skip/omit.txt" ]; then
    fail "tar extracted a file that should have been excluded"
fi

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
