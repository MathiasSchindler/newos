#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/metadata"
note "phase1 filesystem metadata"

touch "$WORK_DIR/mode.txt"
assert_command_succeeds "$ROOT_DIR/build/chmod" 600 "$WORK_DIR/mode.txt"
mode_out=$("$ROOT_DIR/build/stat" -c '%a' "$WORK_DIR/mode.txt" | tr -d '\r\n')
assert_text_equals "$mode_out" '600' "chmod numeric mode did not set the expected permissions"

assert_command_succeeds "$ROOT_DIR/build/chmod" u+x "$WORK_DIR/mode.txt"
mode_exec_out=$("$ROOT_DIR/build/stat" -c '%a' "$WORK_DIR/mode.txt" | tr -d '\r\n')
assert_text_equals "$mode_exec_out" '700' "chmod symbolic mode did not add execute permission for the owner"

assert_command_succeeds "$ROOT_DIR/build/mktemp" -p "$WORK_DIR" phase1.XXXXXX > "$WORK_DIR/mktemp_path.out"
temp_file=$(tr -d '\r\n' < "$WORK_DIR/mktemp_path.out")
[ -f "$temp_file" ] || fail "mktemp did not create the requested file"

assert_command_succeeds "$ROOT_DIR/build/mktemp" -d -p "$WORK_DIR" phase1dir.XXXXXX > "$WORK_DIR/mktemp_dir.out"
temp_dir=$(tr -d '\r\n' < "$WORK_DIR/mktemp_dir.out")
[ -d "$temp_dir" ] || fail "mktemp -d did not create the requested directory"

assert_command_succeeds "$ROOT_DIR/build/mktemp" -u -p "$WORK_DIR" preview.XXXXXX > "$WORK_DIR/mktemp_dry.out"
dry_path=$(tr -d '\r\n' < "$WORK_DIR/mktemp_dry.out")
[ ! -e "$dry_path" ] || fail "mktemp -u unexpectedly created the path"

printf 'abcdef\n' > "$WORK_DIR/dd_input.txt"
assert_command_succeeds "$ROOT_DIR/build/dd" if="$WORK_DIR/dd_input.txt" of="$WORK_DIR/dd_output.txt" bs=3 count=2 status=none
dd_out=$(tr -d '\r\n' < "$WORK_DIR/dd_output.txt")
assert_text_equals "$dd_out" 'abcdef' "dd did not copy the expected data blocks"

chown_status=0
"$ROOT_DIR/build/chown" nosuchuser "$WORK_DIR/mode.txt" > "$WORK_DIR/chown.out" 2>&1 || chown_status=$?
assert_text_equals "$chown_status" '1' "chown with an invalid owner should fail"
assert_file_contains "$WORK_DIR/chown.out" 'invalid owner spec' "chown did not explain the invalid owner failure"
