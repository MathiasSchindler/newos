#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/cp"
note "phase1 filesystem cp"

mkdir -p "$WORK_DIR/src/sub" "$WORK_DIR/multi"
printf 'alpha\n' > "$WORK_DIR/src/root.txt"
printf 'beta\n' > "$WORK_DIR/src/sub/file.txt"

assert_command_succeeds "$ROOT_DIR/build/cp" -r "$WORK_DIR/src" "$WORK_DIR/tree_copy"
assert_file_contains "$WORK_DIR/tree_copy/sub/file.txt" '^beta$' "cp -r did not preserve nested file contents"

assert_command_succeeds "$ROOT_DIR/build/touch" -t 200102030405.06 "$WORK_DIR/src/root.txt"
assert_command_succeeds "$ROOT_DIR/build/cp" -p "$WORK_DIR/src/root.txt" "$WORK_DIR/preserved.txt"
cp_src_mtime=$("$ROOT_DIR/build/stat" -c '%Y' "$WORK_DIR/src/root.txt" | tr -d '\r\n')
cp_dest_mtime=$("$ROOT_DIR/build/stat" -c '%Y' "$WORK_DIR/preserved.txt" | tr -d '\r\n')
assert_text_equals "$cp_dest_mtime" "$cp_src_mtime" "cp -p did not preserve the modification time"

printf 'A\n' > "$WORK_DIR/src/a.txt"
printf 'B\n' > "$WORK_DIR/src/b.txt"
assert_command_succeeds "$ROOT_DIR/build/cp" "$WORK_DIR/src/a.txt" "$WORK_DIR/src/b.txt" "$WORK_DIR/multi"
[ -f "$WORK_DIR/multi/a.txt" ] || fail "cp multi-source mode missed the first file"
[ -f "$WORK_DIR/multi/b.txt" ] || fail "cp multi-source mode missed the second file"

printf 'keep\n' > "$WORK_DIR/keep.txt"
printf 'replace\n' > "$WORK_DIR/replace.txt"
assert_command_succeeds "$ROOT_DIR/build/cp" -n "$WORK_DIR/replace.txt" "$WORK_DIR/keep.txt"
keep_text=$(tr -d '\r\n' < "$WORK_DIR/keep.txt")
assert_text_equals "$keep_text" 'keep' "cp -n overwrote an existing destination"
