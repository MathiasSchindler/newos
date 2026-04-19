#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/touch"
note "phase1 filesystem touch"

touch_target="$WORK_DIR/flags.txt"
printf 'touch\n' > "$touch_target"
touch_original_m=$("$ROOT_DIR/build/stat" -c '%Y' "$touch_target" | tr -d '\r\n')

assert_command_succeeds "$ROOT_DIR/build/touch" -a -d @1111111111 "$touch_target"
touch_after_a_only=$("$ROOT_DIR/build/stat" -c '%X %Y' "$touch_target" | tr -d '\r\n')
assert_text_equals "$touch_after_a_only" "1111111111 $touch_original_m" "touch -a changed the wrong timestamps"

assert_command_succeeds "$ROOT_DIR/build/touch" -m -d @1234567890 "$touch_target"
touch_after_m_only=$("$ROOT_DIR/build/stat" -c '%X %Y' "$touch_target" | tr -d '\r\n')
assert_text_equals "$touch_after_m_only" '1111111111 1234567890' "touch -m did not preserve atime"

assert_command_succeeds "$ROOT_DIR/build/touch" -c "$WORK_DIR/not-created.txt"
[ ! -e "$WORK_DIR/not-created.txt" ] || fail "touch -c unexpectedly created a missing file"

assert_command_succeeds "$ROOT_DIR/build/touch" -d @1234567890 "$WORK_DIR/reference.txt"
assert_command_succeeds "$ROOT_DIR/build/touch" -r "$WORK_DIR/reference.txt" "$WORK_DIR/copied.txt"
reference_times=$("$ROOT_DIR/build/stat" -c '%X %Y' "$WORK_DIR/reference.txt" | tr -d '\r\n')
copied_times=$("$ROOT_DIR/build/stat" -c '%X %Y' "$WORK_DIR/copied.txt" | tr -d '\r\n')
assert_text_equals "$copied_times" "$reference_times" "touch -r did not copy the reference timestamps"

assert_command_succeeds "$ROOT_DIR/build/touch" -t 202401020304.05 "$WORK_DIR/stamp.txt"
touch_stamp_time=$("$ROOT_DIR/build/stat" -c '%Y' "$WORK_DIR/stamp.txt" | tr -d '\r\n')
assert_text_equals "$touch_stamp_time" '1704164645' "touch -t did not parse the fixed timestamp correctly"
