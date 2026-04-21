#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup date

"$ROOT_DIR/build/date" > "$WORK_DIR/date.out"
assert_file_contains "$WORK_DIR/date.out" 'UTC$' "date default output was missing the UTC suffix"

date_fmt_out=$("$ROOT_DIR/build/date" +%Y-%m-%d | tr -d '\r\n')
printf '%s\n' "$date_fmt_out" > "$WORK_DIR/date_fmt.out"
assert_file_contains "$WORK_DIR/date_fmt.out" '^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$' "date +FORMAT output was malformed"

date_epoch_out=$("$ROOT_DIR/build/date" -d @0 +%Y-%m-%dT%H:%M:%S | tr -d '\r\n')
assert_text_equals "$date_epoch_out" '1970-01-01T00:00:00' "date -d @0 did not honor the requested epoch"

printf 'reference\n' > "$WORK_DIR/reference.txt"
touch -t 200001021234 "$WORK_DIR/reference.txt"
date_reference_out=$("$ROOT_DIR/build/date" -l -r "$WORK_DIR/reference.txt" +%Y-%m-%d | tr -d '\r\n')
assert_text_equals "$date_reference_out" '2000-01-02' "date -r did not read the file timestamp"

date_invalid_status=0
"$ROOT_DIR/build/date" -d @999999999999999999999999 >/dev/null 2>"$WORK_DIR/date_invalid.err" || date_invalid_status=$?
assert_exit_code "$date_invalid_status" '1' "date should reject out-of-range epoch values"
assert_file_contains "$WORK_DIR/date_invalid.err" '^date: unsupported date value:' "date did not report the invalid epoch cleanly"
