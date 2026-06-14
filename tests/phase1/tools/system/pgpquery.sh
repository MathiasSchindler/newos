#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup pgpquery

"${TEST_BIN_DIR}/pgpquery" --help > "$WORK_DIR/help.out" 2>&1
assert_file_contains "$WORK_DIR/help.out" '^Usage: pgpquery ' "pgpquery --help did not print usage"
assert_file_contains "$WORK_DIR/help.out" '.*--print-url' "pgpquery usage did not mention --print-url"

"${TEST_BIN_DIR}/pgpquery" --print-url --server all 0x99D37C39FA2C23A8 test@example.com > "$WORK_DIR/urls.out"
assert_file_contains "$WORK_DIR/urls.out" '^keys.openpgp.org: https://keys.openpgp.org/vks/v1/by-keyid/99d37c39fa2c23a8$' "pgpquery did not normalize key IDs for keys.openpgp.org"
assert_file_contains "$WORK_DIR/urls.out" '^keyserver.ubuntu.com: https://keyserver.ubuntu.com/pks/lookup?op=index&options=mr&search=0x99d37c39fa2c23a8$' "pgpquery did not normalize key IDs for HKP index lookup"
assert_file_contains "$WORK_DIR/urls.out" '^keys.openpgp.org: https://keys.openpgp.org/vks/v1/by-email/test%40example.com$' "pgpquery did not URL-encode email selectors for keys.openpgp.org"
assert_file_contains "$WORK_DIR/urls.out" '^keyserver.ubuntu.com: https://keyserver.ubuntu.com/pks/lookup?op=index&options=mr&search=test%40example.com$' "pgpquery did not URL-encode email selectors for HKP index lookup"

"${TEST_BIN_DIR}/pgpquery" --json --print-url --server ubuntu 99d37c39fa2c23a8 > "$WORK_DIR/url.jsonl"
assert_file_contains "$WORK_DIR/url.jsonl" '"schema":"newos.tool.v1"' "pgpquery --json --print-url did not use the shared JSON envelope"
assert_file_contains "$WORK_DIR/url.jsonl" '"event":"query_url"' "pgpquery --json --print-url did not emit query_url events"
assert_file_contains "$WORK_DIR/url.jsonl" '"server":"keyserver.ubuntu.com"' "pgpquery JSON did not report the selected server"
assert_file_contains "$WORK_DIR/url.jsonl" '"url":"https://keyserver.ubuntu.com/pks/lookup?op=index&options=mr&search=0x99d37c39fa2c23a8"' "pgpquery JSON did not report the generated URL"

"${TEST_BIN_DIR}/pgpquery" --get --print-url 99d37c39fa2c23a8 > "$WORK_DIR/get_url.out"
assert_file_contains "$WORK_DIR/get_url.out" '^keyserver.ubuntu.com: https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x99d37c39fa2c23a8$' "pgpquery --get did not select the HKP get endpoint"
if grep 'keys.openpgp.org' "$WORK_DIR/get_url.out" >/dev/null 2>&1; then
    fail "pgpquery --get unexpectedly printed a keys.openpgp.org URL"
fi

if "${TEST_BIN_DIR}/pgpquery" --print-url --server ubuntu 'not a selector' > "$WORK_DIR/invalid.out" 2> "$WORK_DIR/invalid.err"; then
    fail "pgpquery accepted an invalid selector"
fi
assert_file_contains "$WORK_DIR/invalid.err" 'unsupported selector' "pgpquery did not explain invalid selectors"
