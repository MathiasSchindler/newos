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

printf 'older-source\n' > "$WORK_DIR/update_src.txt"
assert_command_succeeds "$ROOT_DIR/build/touch" -d @1000000000 "$WORK_DIR/update_src.txt"
printf 'newer-destination\n' > "$WORK_DIR/update_dest.txt"
assert_command_succeeds "$ROOT_DIR/build/touch" -d @2000000000 "$WORK_DIR/update_dest.txt"
assert_command_succeeds "$ROOT_DIR/build/cp" -u "$WORK_DIR/update_src.txt" "$WORK_DIR/update_dest.txt"
update_text=$(tr -d '\r\n' < "$WORK_DIR/update_dest.txt")
assert_text_equals "$update_text" 'newer-destination' "cp -u replaced a newer destination"

printf 'archive-data\n' > "$WORK_DIR/archive-target.txt"
ln -sf archive-target.txt "$WORK_DIR/archive-link"
mkdir -p "$WORK_DIR/archive-dest"
assert_command_succeeds "$ROOT_DIR/build/cp" -a "$WORK_DIR/archive-link" "$WORK_DIR/archive-dest"
[ -L "$WORK_DIR/archive-dest/archive-link" ] || fail "cp -a did not preserve the symlink itself"
archive_link_out=$("$ROOT_DIR/build/readlink" "$WORK_DIR/archive-dest/archive-link" | tr -d '\r\n')
assert_text_equals "$archive_link_out" 'archive-target.txt' "cp -a preserved the wrong symlink target"

DEEP_SRC="$WORK_DIR/deep_src"
DEEP_REL=""
level=1
while [ "$level" -le 12 ]; do
    DEEP_REL="$DEEP_REL/level_$level"
    mkdir -p "$DEEP_SRC$DEEP_REL"
    level=$((level + 1))
done
printf 'deep-data\n' > "$DEEP_SRC$DEEP_REL/final.txt"

assert_command_succeeds "$ROOT_DIR/build/cp" -r "$DEEP_SRC" "$WORK_DIR/deep_copy"
assert_file_contains "$WORK_DIR/deep_copy$DEEP_REL/final.txt" 'deep-data' "cp -r did not preserve a deeply nested file"

ODD_FILE="$WORK_DIR/odd name [v1] !.txt"
printf 'odd-data\n' > "$ODD_FILE"
assert_command_succeeds "$ROOT_DIR/build/cp" "$ODD_FILE" "$WORK_DIR/odd-copy.txt"
assert_file_contains "$WORK_DIR/odd-copy.txt" '^odd-data$' "cp did not handle an odd filename safely"

printf 'self-data\n' > "$WORK_DIR/self-target.txt"
ln -sf self-target.txt "$WORK_DIR/self-link.txt"
self_copy_status=0
"$ROOT_DIR/build/cp" "$WORK_DIR/self-link.txt" "$WORK_DIR/self-target.txt" >/dev/null 2>&1 || self_copy_status=$?
[ "$self_copy_status" -ne 0 ] || fail "cp should refuse to overwrite a file through a symlink to itself"
assert_file_contains "$WORK_DIR/self-target.txt" '^self-data$' "cp self-copy protection corrupted the original file"
