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
printf '<\303\251/>\n' > "$work_dir/utf8-name-two.xml"
printf '<\346\274\242/>\n' > "$work_dir/utf8-name-three.xml"
printf '<\360\240\200\200/>\n' > "$work_dir/utf8-name-four.xml"
printf '<\303\251><\303\251/></\303\251>\n' > "$work_dir/utf8-name-two-paired.xml"
printf '<\346\274\242><\346\274\242/></\346\274\242>\n' > "$work_dir/utf8-name-three-paired.xml"
printf '<\360\240\200\200><\360\240\200\200/></\360\240\200\200>\n' > "$work_dir/utf8-name-four-paired.xml"
printf '<?xml version="1.0" encoding="UTF-8"?>\n<\303\251/>\n' > "$work_dir/utf8-name-declaration.xml"
printf '<\303\251:r xmlns:\303\251="urn:r"><\346\274\242/></\303\251:r>\n' > "$work_dir/utf8-name-qualified.xml"
printf '<a\303\251 a="\346\274\242">\360\240\200\200</a\303\251>\n' > "$work_dir/utf8-name-mixed.xml"
printf '<\303\251 \346\274\242="v">\360\240\200\200<\303\251/><\360\240\200\200/><!--\303\251--><![CDATA[\346\274\242]]><?\303\251 x?></\303\251>\n' > "$work_dir/utf8-name-events.xml"
printf '<?xml version="1.0"?><!DOCTYPE root [<!ELEMENT root ANY>]><root><!--\303\251--><![CDATA[\346\274\242]]><?\303\251 x?></root>\n' > "$work_dir/utf8-markup.xml"
{
    printf '<\303\251'
    awk 'BEGIN { for (i = 0; i < 260; ++i) printf "a"; }'
    printf '/>\n'
} > "$work_dir/utf8-name-long.xml"
# Place the UTF-8 lead or preceding '<' at offset 8191, the buffer's last byte.
for boundary in 8184 8185; do
    for utf8_width in two three four; do
        {
            printf '<root>'
            awk -v padding="$boundary" 'BEGIN { for (i = 0; i < padding; ++i) printf "x"; }'
            case "$utf8_width" in
                two) printf '<\303\251/>' ;;
                three) printf '<\346\274\242/>' ;;
                four) printf '<\360\240\200\200/>' ;;
            esac
            printf '</root>\n'
        } > "$work_dir/utf8-boundary-$boundary-$utf8_width.xml"
    done
done
for parse_mode in --buffered --stream; do
    for valid_utf8 in "$work_dir/predicate-utf8-name.xml" "$work_dir"/utf8-name-*.xml "$work_dir"/utf8-boundary-*.xml "$work_dir/utf8-markup.xml"; do
        assert_command_succeeds "${TEST_BIN_DIR}/xmlcheck" "$parse_mode" "$valid_utf8"
    done
done
assert_command_succeeds "${TEST_BIN_DIR}/xmlcheck" "$work_dir/utf8-name-two.xml"
printf '<\303\251/>\n' | "${TEST_BIN_DIR}/xmlcheck" --stream

"${TEST_BIN_DIR}/xmlcount" "$work_dir/utf8-name-events.xml" > "$work_dir/utf8-events-count.out"
printf 'elements 3\nattributes 1\ntext 2\ncomments 1\ncdata 1\npi 1\ndoctype 0\nmax-depth 2\n' > "$work_dir/utf8-events-count.expected"
assert_files_equal "$work_dir/utf8-events-count.expected" "$work_dir/utf8-events-count.out" "XML streaming events changed for UTF-8 names or markup"
"${TEST_BIN_DIR}/xmlcount" "$work_dir/utf8-markup.xml" > "$work_dir/utf8-markup-count.out"
printf 'elements 1\nattributes 0\ntext 1\ncomments 1\ncdata 1\npi 2\ndoctype 1\nmax-depth 1\n' > "$work_dir/utf8-markup-count.expected"
assert_files_equal "$work_dir/utf8-markup-count.expected" "$work_dir/utf8-markup-count.out" "XML streaming markup dispatch changed event counts"
"${TEST_BIN_DIR}/xmlstats" "$work_dir/utf8-name-events.xml" > "$work_dir/utf8-events-stats.out"
assert_file_contains "$work_dir/utf8-events-stats.out" '^elements 3$' "xmlstats lost UTF-8 elements"
assert_file_contains "$work_dir/utf8-events-stats.out" '^attributes 1$' "xmlstats lost a UTF-8 attribute"
assert_file_contains "$work_dir/utf8-events-stats.out" '^text-bytes 7$' "xmlstats changed UTF-8 text byte counts"
assert_file_contains "$work_dir/utf8-events-stats.out" "$(printf '^    \303\251 2$')" "xmlstats changed a two-byte UTF-8 element name"
assert_file_contains "$work_dir/utf8-events-stats.out" "$(printf '^    \360\240\200\200 1$')" "xmlstats changed a four-byte UTF-8 element name"
assert_file_contains "$work_dir/utf8-events-stats.out" "$(printf '^    \346\274\242 1$')" "xmlstats changed a three-byte UTF-8 attribute name"
for parse_mode in --buffered --stream; do
    assert_command_succeeds "${TEST_BIN_DIR}/xmlsafe" "$parse_mode" "$work_dir/utf8-name-two-paired.xml"
    assert_command_succeeds "${TEST_BIN_DIR}/xmlsafe" "$parse_mode" --allow-comments --allow-pi "$work_dir/utf8-name-events.xml"
    if "${TEST_BIN_DIR}/xmlsafe" "$parse_mode" "$work_dir/utf8-name-events.xml" > /dev/null 2> "$work_dir/utf8-events-safe.err"; then
        fail "xmlsafe accepted a disallowed comment after a UTF-8 element name"
    fi
    assert_file_contains "$work_dir/utf8-events-safe.err" "comment is not allowed" "xmlsafe changed comment rejection after a UTF-8 name"
done
assert_command_succeeds "${TEST_BIN_DIR}/xmlvalidate" --stream --root "$(printf '\303\251')" "$work_dir/utf8-name-two-paired.xml"

printf '<\200/>\n' > "$work_dir/utf8-invalid-continuation.xml"
printf '<\300\257/>\n' > "$work_dir/utf8-invalid-overlong.xml"
printf '<\355\240\200/>\n' > "$work_dir/utf8-invalid-surrogate.xml"
printf '<\364\220\200\200/>\n' > "$work_dir/utf8-invalid-range.xml"
printf '<\303/>\n' > "$work_dir/utf8-invalid-truncated.xml"
printf '<\343\200A/>\n' > "$work_dir/utf8-invalid-interrupted.xml"
for parse_mode in --buffered --stream; do
    for invalid_utf8 in "$work_dir"/utf8-invalid-*.xml; do
        if "${TEST_BIN_DIR}/xmlcheck" "$parse_mode" "$invalid_utf8" > /dev/null 2> "$work_dir/utf8-invalid.err"; then
            fail "xmlcheck accepted malformed UTF-8 in an element name"
        fi
        assert_file_contains "$work_dir/utf8-invalid.err" 'invalid UTF-8 sequence' "xmlcheck did not diagnose malformed UTF-8 in an element name"
    done
done

printf '<\303\251>\n<\303x/></\303\251>\n' > "$work_dir/utf8-diagnostic.xml"
for parse_mode in --buffered --stream; do
    if "${TEST_BIN_DIR}/xmlcheck" "$parse_mode" "$work_dir/utf8-diagnostic.xml" > /dev/null 2> "$work_dir/utf8-diagnostic.err"; then
        fail "xmlcheck accepted malformed UTF-8 after a valid UTF-8 element name"
    fi
    assert_file_contains "$work_dir/utf8-diagnostic.err" ':2:2: invalid UTF-8 sequence' "xmlcheck changed the UTF-8 error location"
done

printf '<\303' > "$work_dir/unfinished-utf8-name.xml"
for parse_mode in --buffered --stream; do
    for unfinished_markup in '<' '</' '<?' '<!' '<!-' '<![' '<!D' '<1root/>' '< root/>'; do
        printf '%s' "$unfinished_markup" > "$work_dir/unfinished-markup.xml"
        if "${TEST_BIN_DIR}/xmlcheck" "$parse_mode" "$work_dir/unfinished-markup.xml" > /dev/null 2> "$work_dir/unfinished-markup.err"; then
            fail "xmlcheck accepted invalid or unfinished markup"
        fi
        [ -s "$work_dir/unfinished-markup.err" ] || fail "xmlcheck did not diagnose invalid or unfinished markup"
    done
    if "${TEST_BIN_DIR}/xmlcheck" "$parse_mode" "$work_dir/unfinished-utf8-name.xml" > /dev/null 2> "$work_dir/unfinished-markup.err"; then
        fail "xmlcheck accepted an unfinished UTF-8 element name"
    fi
    [ -s "$work_dir/unfinished-markup.err" ] || fail "xmlcheck did not diagnose an unfinished UTF-8 element name"
done

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