#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "filesystem/touch"
note "phase1 filesystem touch"

touch_data_dir="$WORK_DIR"
if command -v mktemp >/dev/null 2>&1; then
    temp_touch_dir=$(mktemp -d "${TMPDIR:-/tmp}/newos-touch-XXXXXX" 2>/dev/null || true)
    if [ -n "${temp_touch_dir:-}" ] && [ -d "$temp_touch_dir" ]; then
        touch_data_dir="$temp_touch_dir"
        trap 'rm -rf "$touch_data_dir"' EXIT HUP INT TERM
    fi
fi

touch_target="$touch_data_dir/flags.txt"
printf 'touch\n' > "$touch_target"
assert_command_succeeds "$ROOT_DIR/build/touch" -d @1770000000 "$touch_target"
touch_original_m=1770000000

assert_command_succeeds "$ROOT_DIR/build/touch" -a -d @1111111111 "$touch_target"
touch_after_a_only=$("$ROOT_DIR/build/stat" -c '%X %Y' "$touch_target" | tr -d '\r\n')
assert_text_equals "$touch_after_a_only" "1111111111 $touch_original_m" "touch -a changed the wrong timestamps"

assert_command_succeeds "$ROOT_DIR/build/touch" -m -d @1234567890 "$touch_target"
touch_after_m_only=$("$ROOT_DIR/build/stat" -c '%X %Y' "$touch_target" | tr -d '\r\n')
assert_text_equals "$touch_after_m_only" '1111111111 1234567890' "touch -m did not preserve atime"

assert_command_succeeds "$ROOT_DIR/build/touch" -c "$touch_data_dir/not-created.txt"
[ ! -e "$touch_data_dir/not-created.txt" ] || fail "touch -c unexpectedly created a missing file"

assert_command_succeeds "$ROOT_DIR/build/touch" --no-create "$touch_data_dir/not-created-long.txt"
[ ! -e "$touch_data_dir/not-created-long.txt" ] || fail "touch --no-create unexpectedly created a missing file"

assert_command_succeeds "$ROOT_DIR/build/touch" -d @1234567890 "$touch_data_dir/reference.txt"
assert_command_succeeds "$ROOT_DIR/build/touch" -r "$touch_data_dir/reference.txt" "$touch_data_dir/copied.txt"
reference_times=$("$ROOT_DIR/build/stat" -c '%X %Y' "$touch_data_dir/reference.txt" | tr -d '\r\n')
copied_times=$("$ROOT_DIR/build/stat" -c '%X %Y' "$touch_data_dir/copied.txt" | tr -d '\r\n')
assert_text_equals "$copied_times" "$reference_times" "touch -r did not copy the reference timestamps"

assert_command_succeeds "$ROOT_DIR/build/touch" --time=modify --date=@1357924680 "$touch_data_dir/long-form.txt"
touch_long_form_time=$("$ROOT_DIR/build/stat" -c '%Y' "$touch_data_dir/long-form.txt" | tr -d '\r\n')
assert_text_equals "$touch_long_form_time" '1357924680' "touch long-form aliases did not set the requested modification time"

assert_command_succeeds "$ROOT_DIR/build/touch" -t 202401020304.05 "$touch_data_dir/stamp.txt"
touch_stamp_time=$("$ROOT_DIR/build/stat" -c '%Y' "$touch_data_dir/stamp.txt" | tr -d '\r\n')
assert_text_equals "$touch_stamp_time" '1704164645' "touch -t did not parse the fixed timestamp correctly"
