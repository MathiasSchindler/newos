#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup pdfinfo

cat > "$WORK_DIR/sample.pdf" <<'EOF'
%PDF-1.7
1 0 obj
<< /Type /Catalog /Pages 2 0 R >>
endobj
2 0 obj
<< /Type /Pages /Kids [3 0 R] /Count 1 >>
endobj
3 0 obj
<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Rotate 0 /Resources << /Font << /F1 4 0 R >> /XObject << /Im1 6 0 R >> >> /Contents 5 0 R >>
endobj
4 0 obj
<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>
endobj
5 0 obj
<< /Length 65 >>
stream
BT /F1 12 Tf 72 720 Td (Hello PDF) Tj ET
0 0 100 100 re S
/Im1 Do
endstream
endobj
6 0 obj
<< /Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length 8 >>
stream
compressed
endstream
endobj
xref
0 7
0000000000 65535 f 
trailer
<< /Root 1 0 R /Size 7 >>
startxref
0
%%EOF
EOF

"${TEST_BIN_DIR}/pdfinfo" --plain "$WORK_DIR/sample.pdf" > "$WORK_DIR/plain.txt"
assert_file_contains "$WORK_DIR/plain.txt" 'version=1.7' "pdfinfo --plain did not report version"
assert_file_contains "$WORK_DIR/plain.txt" 'pages=1' "pdfinfo --plain did not count pages"
assert_file_contains "$WORK_DIR/plain.txt" 'objects=6' "pdfinfo --plain did not count objects"
assert_file_contains "$WORK_DIR/plain.txt" 'streams=2' "pdfinfo --plain did not count streams"
assert_file_contains "$WORK_DIR/plain.txt" 'fonts=1' "pdfinfo --plain did not count fonts"
assert_file_contains "$WORK_DIR/plain.txt" 'images=1' "pdfinfo --plain did not count images"

"${TEST_BIN_DIR}/pdfinfo" --details "$WORK_DIR/sample.pdf" > "$WORK_DIR/details.txt"
assert_file_contains "$WORK_DIR/details.txt" 'filters: FlateDecode(1)' "pdfinfo did not count visible filters"
assert_file_contains "$WORK_DIR/details.txt" 'encodings: WinAnsiEncoding(1)' "pdfinfo did not count font encodings"
assert_file_contains "$WORK_DIR/details.txt" 'font_names: Helvetica(1)' "pdfinfo did not count font names"
assert_file_contains "$WORK_DIR/details.txt" 'media=612x792pt format=Letter' "pdfinfo did not report Letter page dimensions"
assert_file_contains "$WORK_DIR/details.txt" 'subtype=Type1 base=Helvetica encoding=WinAnsiEncoding' "pdfinfo did not list font details"
assert_file_contains "$WORK_DIR/details.txt" 'text_objects: 1' "pdfinfo did not count text objects"
assert_file_contains "$WORK_DIR/details.txt" 'text_show_ops: 1' "pdfinfo did not count text show operators"
assert_file_contains "$WORK_DIR/details.txt" 'path_ops: 2' "pdfinfo did not count path operators"
assert_file_contains "$WORK_DIR/details.txt" 'xobject_paint_ops: 1' "pdfinfo did not count XObject paint operators"

"${TEST_BIN_DIR}/pdfinfo" --objects "$WORK_DIR/sample.pdf" > "$WORK_DIR/objects.txt"
assert_file_contains "$WORK_DIR/objects.txt" 'obj 6 0 offset=' "pdfinfo --objects did not list image object"
assert_file_contains "$WORK_DIR/objects.txt" 'type=XObject subtype=Image stream=yes' "pdfinfo --objects did not report image object metadata"

"${TEST_BIN_DIR}/pdfinfo" --json "$WORK_DIR/sample.pdf" > "$WORK_DIR/json.txt"
assert_file_contains "$WORK_DIR/json.txt" '"pages":1' "pdfinfo --json did not report pages"
assert_file_contains "$WORK_DIR/json.txt" '"objects":6' "pdfinfo --json did not report objects"
assert_file_contains "$WORK_DIR/json.txt" '"name":"FlateDecode"' "pdfinfo --json did not report filters"
assert_file_contains "$WORK_DIR/json.txt" '"name":"WinAnsiEncoding"' "pdfinfo --json did not report encodings"

assert_file_contains "$WORK_DIR/sample.pdf" '%PDF-1.7' "test fixture sanity check failed"
assert_text_equals "$(cat "$WORK_DIR/sample.pdf" | "${TEST_BIN_DIR}/pdfinfo" --plain)" "stdin version=1.7 bytes=$(wc -c < "$WORK_DIR/sample.pdf" | tr -d ' ') pages=1 objects=6 streams=2 fonts=1 images=1" "pdfinfo did not parse stdin"

printf 'not a pdf\n' > "$WORK_DIR/not-pdf.txt"
if "${TEST_BIN_DIR}/pdfinfo" "$WORK_DIR/not-pdf.txt" >/dev/null 2>&1; then
    fail "pdfinfo should reject unsupported data"
fi