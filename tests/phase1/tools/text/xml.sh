#!/bin/sh
set -eu

. "$(dirname -- "$0")/common.inc"

work_dir=$(phase1_text_workdir xml)

bad_decl="$work_dir/leading-whitespace-declaration.xml"
bad_decl_stream_err="$work_dir/leading-whitespace-declaration-stream.err"
good_xml="$work_dir/basic.xml"
tokens_out="$work_dir/tokens.out"
stats_out="$work_dir/stats.out"
bad_selector_err="$work_dir/bad-selector.err"
bad_count_err="$work_dir/bad-count.err"

printf ' <?xml version="1.0"?><root/>\n' > "$bad_decl"
if "${TEST_BIN_DIR}/xmlcheck" --stream "$bad_decl" > /dev/null 2> "$bad_decl_stream_err"; then
    fail "xmlcheck --stream accepted XML declaration after leading whitespace"
fi
assert_file_contains "$bad_decl_stream_err" "XML declaration must be at document start" "streaming xmlcheck reports leading whitespace before XML declaration"

printf '<root><item id="1">alpha</item><item id="2">beta</item></root>\n' > "$good_xml"
assert_command_succeeds "${TEST_BIN_DIR}/xmlcheck" "$good_xml"
"${TEST_BIN_DIR}/xmltokens" "$good_xml" > "$tokens_out"
assert_file_contains "$tokens_out" "start depth=0 name=root" "xmltokens reports root start token"
assert_file_contains "$tokens_out" "text depth=2 text=\"alpha\"" "xmltokens reports text token"

"${TEST_BIN_DIR}/xmlstats" "$good_xml" > "$stats_out"
assert_file_contains "$stats_out" '^elements 3$' "xmlstats reports element count"
assert_file_contains "$stats_out" '^attributes 2$' "xmlstats reports attribute count"
assert_file_contains "$stats_out" '^    item 2$' "xmlstats reports repeated element names"
assert_file_contains "$stats_out" '^    id 2$' "xmlstats reports repeated attribute names"

if "${TEST_BIN_DIR}/xmlget" '[' "$good_xml" > /dev/null 2> "$bad_selector_err"; then
    fail "xmlget accepted an invalid selector"
fi
assert_file_contains "$bad_selector_err" "xmlget: invalid selector" "xmlget reports invalid selectors"

if "${TEST_BIN_DIR}/xmlhead" -n nope item "$good_xml" > /dev/null 2> "$bad_count_err"; then
    fail "xmlhead accepted a non-numeric count"
fi
assert_file_contains "$bad_count_err" "xmlhead: invalid count" "xmlhead reports invalid count values"

printf '\377\376<\0r\0o\0o\0t\0>\0o\0k\0<\0/\0r\0o\0o\0t\0>\0' > "$work_dir/utf16le.xml"
"${TEST_BIN_DIR}/xmltokens" "$work_dir/utf16le.xml" > "$work_dir/utf16le.out"
assert_file_contains "$work_dir/utf16le.out" 'text depth=1 text="ok"' "XML tools did not transcode UTF-16LE input"

printf '<?xml version="1.0" encoding="windows-1252"?><root>caf\351</root>\n' > "$work_dir/windows1252.xml"
"${TEST_BIN_DIR}/xmltokens" "$work_dir/windows1252.xml" > "$work_dir/windows1252.out"
assert_file_contains "$work_dir/windows1252.out" 'text depth=1 text="café"' "XML tools did not transcode Windows-1252 input"