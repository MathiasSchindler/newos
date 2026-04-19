#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/mv"
note "phase1 filesystem mv"

mkdir -p "$WORK_DIR/src" "$WORK_DIR/dest"
printf 'A\n' > "$WORK_DIR/src/a.txt"
printf 'B\n' > "$WORK_DIR/src/b.txt"

assert_command_succeeds "$ROOT_DIR/build/mv" "$WORK_DIR/src/a.txt" "$WORK_DIR/src/b.txt" "$WORK_DIR/dest"
[ -f "$WORK_DIR/dest/a.txt" ] || fail "mv multi-source mode missed the first file"
[ -f "$WORK_DIR/dest/b.txt" ] || fail "mv multi-source mode missed the second file"
[ ! -e "$WORK_DIR/src/a.txt" ] || fail "mv left the first source in place"

mkdir -p "$WORK_DIR/self_guard/child"
if "$ROOT_DIR/build/mv" "$WORK_DIR/self_guard" "$WORK_DIR/self_guard/child" >/dev/null 2>&1; then
    fail "mv should refuse to move a directory into itself"
fi
[ -d "$WORK_DIR/self_guard" ] || fail "mv self-guard removed the original directory"

printf 'old-source\n' > "$WORK_DIR/update_src.txt"
printf 'new-dest\n' > "$WORK_DIR/update_dest.txt"
assert_command_succeeds "$ROOT_DIR/build/touch" -d @1000000000 "$WORK_DIR/update_src.txt"
assert_command_succeeds "$ROOT_DIR/build/touch" -d @2000000000 "$WORK_DIR/update_dest.txt"
assert_command_succeeds "$ROOT_DIR/build/mv" -u "$WORK_DIR/update_src.txt" "$WORK_DIR/update_dest.txt"
[ -f "$WORK_DIR/update_src.txt" ] || fail "mv -u moved an older source that should have been skipped"
update_text=$(tr -d '\r\n' < "$WORK_DIR/update_dest.txt")
assert_text_equals "$update_text" 'new-dest' "mv -u replaced a newer destination"

printf 'fresh-source\n' > "$WORK_DIR/update_src.txt"
assert_command_succeeds "$ROOT_DIR/build/touch" -d @3000000000 "$WORK_DIR/update_src.txt"
assert_command_succeeds "$ROOT_DIR/build/mv" -u "$WORK_DIR/update_src.txt" "$WORK_DIR/update_dest.txt"
[ ! -e "$WORK_DIR/update_src.txt" ] || fail "mv -u left the source file behind"
assert_file_contains "$WORK_DIR/update_dest.txt" '^fresh-source$' "mv -u did not replace an older destination"
