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
<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Metadata 0 0 R >>
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
if grep -q 'dangling ref 0 0 R' "$WORK_DIR/valid.txt"; then
    fail "pdfcheck should treat 0 0 R as a null reference"
fi

"${TEST_BIN_DIR}/pdfcheck" --json "$WORK_DIR/valid.pdf" > "$WORK_DIR/valid.json"
assert_file_contains "$WORK_DIR/valid.json" '"ok":true' "pdfcheck --json did not report success"
assert_file_contains "$WORK_DIR/valid.json" '"objects":5' "pdfcheck --json did not include object counters"
assert_file_contains "$WORK_DIR/valid.json" '"streams":1' "pdfcheck --json did not include stream counters"
assert_file_contains "$WORK_DIR/valid.json" '"errors":\[\]' "pdfcheck --json did not include an empty errors array"

pages=80
info_obj=$((3 + pages * 2))
objects=$((info_obj))
: > "$WORK_DIR/scale.pdf"
printf '%%PDF-1.7\n' >> "$WORK_DIR/scale.pdf"
printf '1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n' >> "$WORK_DIR/scale.pdf"
printf '2 0 obj\n<< /Type /Pages /Kids [' >> "$WORK_DIR/scale.pdf"
index=0
while [ "$index" -lt "$pages" ]; do
    printf '%s 0 R ' $((3 + index)) >> "$WORK_DIR/scale.pdf"
    index=$((index + 1))
done
printf '] /Count %s >>\nendobj\n' "$pages" >> "$WORK_DIR/scale.pdf"
index=0
while [ "$index" -lt "$pages" ]; do
    page_obj=$((3 + index))
    stream_obj=$((3 + pages + index))
    printf '%s 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents %s 0 R >>\nendobj\n' "$page_obj" "$stream_obj" >> "$WORK_DIR/scale.pdf"
    printf '%s 0 obj\n<< /Length 45 >>\nstream\nBT (stream text includes fake 9999 0 R) Tj ET\nendstream\nendobj\n' "$stream_obj" >> "$WORK_DIR/scale.pdf"
    index=$((index + 1))
done
printf '%s 0 obj\n<< /Title (Scale Fixture) >>\nendobj\n' "$info_obj" >> "$WORK_DIR/scale.pdf"
printf 'trailer\n<< /Root 1 0 R /Info %s 0 R /Size %s >>\nstartxref\n0\n%%%%EOF\n' "$info_obj" $((objects + 1)) >> "$WORK_DIR/scale.pdf"
"${TEST_BIN_DIR}/pdfcheck" "$WORK_DIR/scale.pdf" > "$WORK_DIR/scale.txt"
assert_file_contains "$WORK_DIR/scale.txt" 'ok: structure checks passed' "pdfcheck did not handle a larger valid reference set"
if grep -q 'dangling ref 9999 0 R' "$WORK_DIR/scale.txt"; then
    fail "pdfcheck should skip reference-like text inside streams in larger files"
fi
"${TEST_BIN_DIR}/pdfcheck" --json "$WORK_DIR/scale.pdf" > "$WORK_DIR/scale.json"
assert_file_contains "$WORK_DIR/scale.json" '"ok":true' "pdfcheck --json did not handle a larger valid reference set"
assert_file_contains "$WORK_DIR/scale.json" '"objects":163' "pdfcheck --json did not count larger fixture objects"

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
