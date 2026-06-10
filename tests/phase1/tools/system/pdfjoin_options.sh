#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup pdfjoin_options

make_pdf() {
    out=$1
    title=$2
    author=$3
    cat > "$out" <<EOF
%PDF-1.7
1 0 obj
<< /Type /Catalog /Pages 2 0 R >>
endobj
2 0 obj
<< /Type /Pages /Kids [3 0 R] /Count 1 >>
endobj
3 0 obj
<< /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R /Type /Page >>
endobj
4 0 obj
<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>
endobj
5 0 obj
<< /Length 41 >>
stream
BT /F1 12 Tf 72 720 Td ($title) Tj ET
endstream
endobj
6 0 obj
<< /Title ($title) /Author ($author) /Subject (Original Subject) /Keywords (original keywords) /Creator (original creator) /Producer (original producer) >>
endobj
xref
0 7
0000000000 65535 f 
trailer
<< /Root 1 0 R /Info 6 0 R /Size 7 >>
startxref
0
%%EOF
EOF
}

make_pdf "$WORK_DIR/one.pdf" "First" "First Author"
make_pdf "$WORK_DIR/two.pdf" "Second" "Second Author"

"${TEST_BIN_DIR}/pdfjoin" \
    -o "$WORK_DIR/override.pdf" \
    --title "Joined Title" \
    --author "Joined Author" \
    --subject "Joined Subject" \
    --keywords "joined keywords" \
    --creator "Joined Creator" \
    --producer "Joined Producer" \
    "$WORK_DIR/one.pdf" "$WORK_DIR/two.pdf"
"${TEST_BIN_DIR}/pdfinfo" --details "$WORK_DIR/override.pdf" > "$WORK_DIR/override.info"
assert_file_contains "$WORK_DIR/override.info" 'pages: 2' "pdfjoin metadata override output did not preserve pages"
assert_file_contains "$WORK_DIR/override.info" 'title: Joined Title' "pdfjoin did not override title metadata"
assert_file_contains "$WORK_DIR/override.info" 'author: Joined Author' "pdfjoin did not override author metadata"
assert_file_contains "$WORK_DIR/override.info" 'subject: Joined Subject' "pdfjoin did not override subject metadata"
assert_file_contains "$WORK_DIR/override.info" 'keywords: joined keywords' "pdfjoin did not override keywords metadata"
assert_file_contains "$WORK_DIR/override.info" 'creator: Joined Creator' "pdfjoin did not override creator metadata"
assert_file_contains "$WORK_DIR/override.info" 'producer: Joined Producer' "pdfjoin did not override producer metadata"

"${TEST_BIN_DIR}/pdfjoin" --no-metadata -o "$WORK_DIR/no-metadata.pdf" "$WORK_DIR/one.pdf" "$WORK_DIR/two.pdf"
"${TEST_BIN_DIR}/pdfinfo" --details "$WORK_DIR/no-metadata.pdf" > "$WORK_DIR/no-metadata.info"
assert_file_contains "$WORK_DIR/no-metadata.info" 'pages: 2' "pdfjoin --no-metadata output did not preserve pages"
assert_file_contains "$WORK_DIR/no-metadata.info" 'info_dictionaries: 0' "pdfjoin --no-metadata wrote document-info metadata"
if grep -q 'title:' "$WORK_DIR/no-metadata.info"; then
    fail "pdfjoin --no-metadata preserved title metadata"
fi

if "${TEST_BIN_DIR}/pdfjoin" --title >/dev/null 2>"$WORK_DIR/missing.err"; then
    fail "pdfjoin --title without a value should fail"
fi
assert_file_contains "$WORK_DIR/missing.err" 'option requires an argument: --title' "pdfjoin missing metadata value error was unclear"

if "${TEST_BIN_DIR}/pdfjoin" --unknown -o "$WORK_DIR/out.pdf" "$WORK_DIR/one.pdf" "$WORK_DIR/two.pdf" >/dev/null 2>"$WORK_DIR/unknown.err"; then
    fail "pdfjoin unknown option should fail"
fi
assert_file_contains "$WORK_DIR/unknown.err" 'unknown option: --unknown' "pdfjoin unknown option error was unclear"

if "${TEST_BIN_DIR}/pdfjoin" --no-metadata --title "No Title" -o "$WORK_DIR/conflict.pdf" "$WORK_DIR/one.pdf" "$WORK_DIR/two.pdf" >/dev/null 2>"$WORK_DIR/conflict.err"; then
    fail "pdfjoin should reject --no-metadata with metadata overrides"
fi
assert_file_contains "$WORK_DIR/conflict.err" 'cannot be combined with metadata overrides' "pdfjoin metadata conflict error was unclear"
