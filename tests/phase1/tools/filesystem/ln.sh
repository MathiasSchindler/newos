#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/ln"
note "phase1 filesystem ln"

printf 'target-a\n' > "$WORK_DIR/target-a.txt"
printf 'target-b\n' > "$WORK_DIR/target-b.txt"

assert_command_succeeds "$ROOT_DIR/build/ln" -sf target-a.txt "$WORK_DIR/force-link"
assert_command_succeeds "$ROOT_DIR/build/ln" -sf target-b.txt "$WORK_DIR/force-link"
force_link_out=$("$ROOT_DIR/build/readlink" "$WORK_DIR/force-link" | tr -d '\r\n')
assert_text_equals "$force_link_out" 'target-b.txt' "ln -f did not replace the existing symlink"

mkdir -p "$WORK_DIR/link-dir"
"$ROOT_DIR/build/ln" -sv "$WORK_DIR/target-a.txt" "$WORK_DIR/target-b.txt" "$WORK_DIR/link-dir" > "$WORK_DIR/verbose.out"
[ -L "$WORK_DIR/link-dir/target-a.txt" ] || fail "ln multi-source mode missed the first link"
[ -L "$WORK_DIR/link-dir/target-b.txt" ] || fail "ln multi-source mode missed the second link"
assert_file_contains "$WORK_DIR/verbose.out" 'link-dir/target-a.txt' "ln -v did not describe the created link"

mkdir -p "$WORK_DIR/relative/real" "$WORK_DIR/relative/links" "$WORK_DIR/hard"
printf 'relative\n' > "$WORK_DIR/relative/real/file.txt"
"$ROOT_DIR/build/ln" -srv "$WORK_DIR/relative/real/file.txt" "$WORK_DIR/relative/links/file.link" > "$WORK_DIR/relative.out"
relative_target=$("$ROOT_DIR/build/readlink" "$WORK_DIR/relative/links/file.link" | tr -d '\r\n')
assert_text_equals "$relative_target" '../real/file.txt' "ln -r did not compute a relative symlink target"

(
    cd "$WORK_DIR/hard" &&
    assert_command_succeeds "$ROOT_DIR/build/ln" ../target-a.txt
)
assert_command_succeeds "$ROOT_DIR/build/test" "$WORK_DIR/target-a.txt" -ef "$WORK_DIR/hard/target-a.txt"
