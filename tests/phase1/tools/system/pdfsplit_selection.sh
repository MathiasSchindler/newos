#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup pdfsplit_selection

cat > "$WORK_DIR/five.pdf" <<'EOF'
%PDF-1.7
1 0 obj
<< /Type /Catalog /Pages 2 0 R >>
endobj
2 0 obj
<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 6 0 R 7 0 R] /Count 5 >>
endobj
3 0 obj
<< /Type /Page /Parent 2 0 R /MediaBox [0 0 101 200] >>
endobj
4 0 obj
<< /Type /Page /Parent 2 0 R /MediaBox [0 0 102 200] >>
endobj
5 0 obj
<< /Type /Page /Parent 2 0 R /MediaBox [0 0 103 200] >>
endobj
6 0 obj
<< /Type /Page /Parent 2 0 R /MediaBox [0 0 104 200] >>
endobj
7 0 obj
<< /Type /Page /Parent 2 0 R /MediaBox [0 0 105 200] >>
endobj
xref
0 8
0000000000 65535 f 
trailer
<< /Root 1 0 R /Size 8 >>
startxref
0
%%EOF
EOF

assert_page_width() {
    info_file=$1
    page_number=$2
    expected_width=$3
    line=$(grep "^  $page_number:" "$info_file" || true)

    case "$line" in
        *"media=${expected_width}x200pt"*)
            ;;
        *)
            fail "expected page $page_number in $info_file to have width $expected_width, got: $line"
            ;;
    esac
}

"${TEST_BIN_DIR}/pdfsplit" --pages 1,3-4,2 -o "$WORK_DIR/selected.pdf" "$WORK_DIR/five.pdf"
"${TEST_BIN_DIR}/pdfinfo" --pages "$WORK_DIR/selected.pdf" > "$WORK_DIR/selected.info"
assert_file_contains "$WORK_DIR/selected.info" 'pages: 4' "multi-selector pdfsplit did not produce four pages"
assert_page_width "$WORK_DIR/selected.info" 1 101
assert_page_width "$WORK_DIR/selected.info" 2 103
assert_page_width "$WORK_DIR/selected.info" 3 104
assert_page_width "$WORK_DIR/selected.info" 4 102

"${TEST_BIN_DIR}/pdfsplit" --pages 4- -o "$WORK_DIR/open-tail.pdf" "$WORK_DIR/five.pdf"
"${TEST_BIN_DIR}/pdfinfo" --pages "$WORK_DIR/open-tail.pdf" > "$WORK_DIR/open-tail.info"
assert_file_contains "$WORK_DIR/open-tail.info" 'pages: 2' "open-ended tail selector did not produce two pages"
assert_page_width "$WORK_DIR/open-tail.info" 1 104
assert_page_width "$WORK_DIR/open-tail.info" 2 105

"${TEST_BIN_DIR}/pdfsplit" --pages -2 -o "$WORK_DIR/open-head.pdf" "$WORK_DIR/five.pdf"
"${TEST_BIN_DIR}/pdfinfo" --pages "$WORK_DIR/open-head.pdf" > "$WORK_DIR/open-head.info"
assert_file_contains "$WORK_DIR/open-head.info" 'pages: 2' "open-ended head selector did not produce two pages"
assert_page_width "$WORK_DIR/open-head.info" 1 101
assert_page_width "$WORK_DIR/open-head.info" 2 102

"${TEST_BIN_DIR}/pdfsplit" --odd -o "$WORK_DIR/odd.pdf" "$WORK_DIR/five.pdf"
"${TEST_BIN_DIR}/pdfinfo" --pages "$WORK_DIR/odd.pdf" > "$WORK_DIR/odd.info"
assert_file_contains "$WORK_DIR/odd.info" 'pages: 3' "odd selector did not produce three pages"
assert_page_width "$WORK_DIR/odd.info" 1 101
assert_page_width "$WORK_DIR/odd.info" 2 103
assert_page_width "$WORK_DIR/odd.info" 3 105

"${TEST_BIN_DIR}/pdfsplit" --even -o "$WORK_DIR/even.pdf" "$WORK_DIR/five.pdf"
"${TEST_BIN_DIR}/pdfinfo" --pages "$WORK_DIR/even.pdf" > "$WORK_DIR/even.info"
assert_file_contains "$WORK_DIR/even.info" 'pages: 2' "even selector did not produce two pages"
assert_page_width "$WORK_DIR/even.info" 1 102
assert_page_width "$WORK_DIR/even.info" 2 104

set +e
"${TEST_BIN_DIR}/pdfsplit" --pages 6 -o "$WORK_DIR/outside.pdf" "$WORK_DIR/five.pdf" 2> "$WORK_DIR/outside.err"
outside_status=$?
set -e
assert_exit_code "$outside_status" 1 "pdfsplit accepted an out-of-range page selector"
assert_file_contains "$WORK_DIR/outside.err" 'page selector outside document (pages: 5)' "out-of-range selector error was not clear"
