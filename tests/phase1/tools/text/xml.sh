#!/bin/sh
set -eu

. "$(dirname -- "$0")/common.inc"

work_dir=$(phase1_text_workdir xml)

bad_decl="$work_dir/leading-whitespace-declaration.xml"
bad_decl_stream_err="$work_dir/leading-whitespace-declaration-stream.err"
good_xml="$work_dir/basic.xml"
tokens_out="$work_dir/tokens.out"

printf ' <?xml version="1.0"?><root/>\n' > "$bad_decl"
if "$ROOT_DIR/build/xmlcheck" --stream "$bad_decl" > /dev/null 2> "$bad_decl_stream_err"; then
    fail "xmlcheck --stream accepted XML declaration after leading whitespace"
fi
assert_file_contains "$bad_decl_stream_err" "XML declaration must be at document start" "streaming xmlcheck reports leading whitespace before XML declaration"

printf '<root><item id="1">alpha</item><item id="2">beta</item></root>\n' > "$good_xml"
assert_command_succeeds "$ROOT_DIR/build/xmlcheck" "$good_xml"
"$ROOT_DIR/build/xmltokens" "$good_xml" > "$tokens_out"
assert_file_contains "$tokens_out" "start depth=0 name=root" "xmltokens reports root start token"
assert_file_contains "$tokens_out" "text depth=2 text=\"alpha\"" "xmltokens reports text token"