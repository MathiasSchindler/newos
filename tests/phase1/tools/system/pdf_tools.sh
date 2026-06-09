#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup pdf_tools

make_pdf() {
    out=$1
    title=$2
    text=$3
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
<< /Length 44 >>
stream
BT /F1 12 Tf 72 720 Td ($text) Tj ET
endstream
endobj
6 0 obj
<< /Title ($title) /Author (Phase One) /Keywords (keep remove) /Producer (newos tests) >>
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

make_pdf "$WORK_DIR/one.pdf" "First" "One"
make_pdf "$WORK_DIR/two.pdf" "Second" "Two"

"${TEST_BIN_DIR}/pdfjoin" -o "$WORK_DIR/joined.pdf" "$WORK_DIR/one.pdf" "$WORK_DIR/two.pdf"
"${TEST_BIN_DIR}/pdfinfo" --details "$WORK_DIR/joined.pdf" > "$WORK_DIR/joined.info"
assert_file_contains "$WORK_DIR/joined.info" 'pages: 2' "pdfjoin did not produce a two-page PDF"
assert_file_contains "$WORK_DIR/joined.info" 'title: First' "pdfjoin did not preserve first document metadata"
assert_file_contains "$WORK_DIR/joined.info" 'fonts: 2' "pdfjoin did not preserve copied font objects"

"${TEST_BIN_DIR}/pdfsplit" --every 1 -o "$WORK_DIR/part" "$WORK_DIR/joined.pdf"
"${TEST_BIN_DIR}/pdfinfo" "$WORK_DIR/part-001.pdf" > "$WORK_DIR/part1.info"
"${TEST_BIN_DIR}/pdfinfo" "$WORK_DIR/part-002.pdf" > "$WORK_DIR/part2.info"
assert_file_contains "$WORK_DIR/part1.info" 'pages: 1' "pdfsplit --every did not create first one-page PDF"
assert_file_contains "$WORK_DIR/part2.info" 'pages: 1' "pdfsplit --every did not create second one-page PDF"

"${TEST_BIN_DIR}/pdfsplit" --pages 2 -o "$WORK_DIR/page2.pdf" "$WORK_DIR/joined.pdf"
"${TEST_BIN_DIR}/pdfinfo" "$WORK_DIR/page2.pdf" > "$WORK_DIR/page2.info"
assert_file_contains "$WORK_DIR/page2.info" 'pages: 1' "pdfsplit --pages did not extract one page"

"${TEST_BIN_DIR}/pdfinfoedit" --set title=Edited --set author=Editor --remove keywords -o "$WORK_DIR/edited.pdf" "$WORK_DIR/one.pdf"
"${TEST_BIN_DIR}/pdfinfo" "$WORK_DIR/edited.pdf" > "$WORK_DIR/edited.info"
assert_file_contains "$WORK_DIR/edited.info" 'title: Edited' "pdfinfoedit did not update title"
assert_file_contains "$WORK_DIR/edited.info" 'author: Editor' "pdfinfoedit did not update author"
if grep -q 'keywords:' "$WORK_DIR/edited.info"; then
    fail "pdfinfoedit did not remove keywords"
fi
