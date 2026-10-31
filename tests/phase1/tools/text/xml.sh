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

printf '<?xml version="1.0" encoding="windows-1252"?><root>\200\201\202\203\204\205\206\207\210\211\212\213\214\215\216\217\220\221\222\223\224\225\226\227\230\231\232\233\234\235\236\237</root>\n' > "$work_dir/windows1252-all.xml"
printf '<?xml version="1.0" encoding="UTF-8"?><root>\342\202\254\302\201\342\200\232\306\222\342\200\236\342\200\246\342\200\240\342\200\241\313\206\342\200\260\305\240\342\200\271\305\222\302\215\305\275\302\217\302\220\342\200\230\342\200\231\342\200\234\342\200\235\342\200\242\342\200\223\342\200\224\313\234\342\204\242\305\241\342\200\272\305\223\302\235\305\276\305\270</root>\n' > "$work_dir/windows1252-all.expected"
for recode_mode in detected explicit; do
    if [ "$recode_mode" = explicit ]; then
        "${TEST_BIN_DIR}/xmlrecode" --from windows-1252 "$work_dir/windows1252-all.xml" > "$work_dir/windows1252-all.out"
    else
        "${TEST_BIN_DIR}/xmlrecode" "$work_dir/windows1252-all.xml" > "$work_dir/windows1252-all.out"
    fi
    assert_files_equal "$work_dir/windows1252-all.expected" "$work_dir/windows1252-all.out" "xmlrecode changed a Windows-1252 mapping or declaration rewrite"
done

printf '<r:_name xmlns:r="urn:r"><n.0-name a="&#x9;">&#xD7FF;&#xE000;&#xFFFD;&#x10000;&#x10FFFF;</n.0-name></r:_name>\n' > "$work_dir/predicate-valid.xml"
printf '<\303\251.0-name/>\n' > "$work_dir/predicate-utf8-name.xml"
assert_command_succeeds "${TEST_BIN_DIR}/xmlcheck" --buffered "$work_dir/predicate-utf8-name.xml"
printf '<root>\013</root>\n' > "$work_dir/predicate-control.xml"
printf '<root>&#xD800;</root>\n' > "$work_dir/predicate-surrogate.xml"
printf '<root>&#x110000;</root>\n' > "$work_dir/predicate-range.xml"
printf '<root>\300\257</root>\n' > "$work_dir/predicate-overlong.xml"
for parse_mode in --buffered --stream; do
    assert_command_succeeds "${TEST_BIN_DIR}/xmlcheck" "$parse_mode" "$work_dir/predicate-valid.xml"
    for invalid_xml in control surrogate range overlong; do
        if "${TEST_BIN_DIR}/xmlcheck" "$parse_mode" "$work_dir/predicate-$invalid_xml.xml" > /dev/null 2> "$work_dir/predicate-invalid.err"; then
            fail "xmlcheck accepted an invalid character or codepoint"
        fi
        [ -s "$work_dir/predicate-invalid.err" ] || fail "xmlcheck did not diagnose invalid character input"
    done
done

awk 'BEGIN {
    for (i = 0; i < 130; ++i) printf "<a>";
    for (i = 0; i < 130; ++i) printf "</a>";
    printf "\n";
}' > "$work_dir/deep-stream.xml"
if "${TEST_BIN_DIR}/xmlcheck" --stream "$work_dir/deep-stream.xml" > /dev/null 2> "$work_dir/deep-stream.err"; then
    fail "xmlcheck --stream accepted nesting beyond default maximum depth"
fi
assert_file_contains "$work_dir/deep-stream.err" "maximum depth exceeded" "streaming xmlcheck enforces default maximum depth"