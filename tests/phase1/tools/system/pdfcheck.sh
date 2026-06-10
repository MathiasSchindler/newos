#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup pdfcheck

cat > "$WORK_DIR/valid.pdf" <<'EOF'
%PDF-1.7
1 0 obj
<< /Type /Catalog /Pages 2 0 R >>
endobj
2 0 obj
<< /Type /Pages /Kids [3 0 R] /Count 1 >>
endobj
3 0 obj
<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>
endobj
4 0 obj
<< /Length 20 >>
stream
BT (Check OK) Tj ET
endstream
endobj
5 0 obj
<< /Title (Check Fixture) >>
endobj
trailer
<< /Root 1 0 R /Info 5 0 R /Size 6 >>
startxref
0
%%EOF
EOF

"${TEST_BIN_DIR}/pdfcheck" "$WORK_DIR/valid.pdf" > "$WORK_DIR/valid.txt"
assert_file_contains "$WORK_DIR/valid.txt" 'ok: header PDF-1.7' "pdfcheck did not accept valid header"
assert_file_contains "$WORK_DIR/valid.txt" 'ok: EOF marker' "pdfcheck did not accept EOF marker"
assert_file_contains "$WORK_DIR/valid.txt" 'root: 1 0 R (ok)' "pdfcheck did not validate root reference"
assert_file_contains "$WORK_DIR/valid.txt" 'info: 5 0 R (ok)' "pdfcheck did not validate info reference"
assert_file_contains "$WORK_DIR/valid.txt" 'ok: structure checks passed' "pdfcheck did not report success"

"${TEST_BIN_DIR}/pdfcheck" --json "$WORK_DIR/valid.pdf" > "$WORK_DIR/valid.json"
assert_file_contains "$WORK_DIR/valid.json" '"ok":true' "pdfcheck --json did not report success"
assert_file_contains "$WORK_DIR/valid.json" '"objects":5' "pdfcheck --json did not include object counters"
assert_file_contains "$WORK_DIR/valid.json" '"streams":1' "pdfcheck --json did not include stream counters"
assert_file_contains "$WORK_DIR/valid.json" '"errors":\[\]' "pdfcheck --json did not include an empty errors array"

cat > "$WORK_DIR/broken.pdf" <<'EOF'
%PDF-1.7
1 0 obj
<< /Type /Catalog /Pages 9 0 R >>
endobj
trailer
<< /Root 1 0 R /Size 2 >>
EOF

if "${TEST_BIN_DIR}/pdfcheck" "$WORK_DIR/broken.pdf" > "$WORK_DIR/broken.txt" 2>&1; then
    fail "pdfcheck should reject a malformed PDF"
fi
assert_file_contains "$WORK_DIR/broken.txt" 'error: missing %%EOF marker' "pdfcheck did not flag missing EOF"
assert_file_contains "$WORK_DIR/broken.txt" 'error: dangling ref 9 0 R' "pdfcheck did not flag dangling page reference"

if "${TEST_BIN_DIR}/pdfcheck" --json "$WORK_DIR/broken.pdf" > "$WORK_DIR/broken.json" 2>&1; then
    fail "pdfcheck --json should reject a malformed PDF"
fi
assert_file_contains "$WORK_DIR/broken.json" '"ok":false' "pdfcheck --json did not report failure"
assert_file_contains "$WORK_DIR/broken.json" '"missing %%EOF marker"' "pdfcheck --json did not include EOF error"
assert_file_contains "$WORK_DIR/broken.json" '"dangling ref 9 0 R"' "pdfcheck --json did not include dangling reference errors"
