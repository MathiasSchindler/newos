#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup pdf_hardening

cat > "$WORK_DIR/huge-length-stream-ref.pdf" <<'EOF'
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
<< /Length 18446744073709551615 >>
stream
BT (stream bytes may contain 99 0 R text) Tj ET
endstream
endobj
trailer
<< /Root 1 0 R /Size 5 >>
startxref
0
%%EOF
EOF

"${TEST_BIN_DIR}/pdfinfo" --plain "$WORK_DIR/huge-length-stream-ref.pdf" > "$WORK_DIR/huge-length-info.txt"
assert_file_contains "$WORK_DIR/huge-length-info.txt" 'pages=1' "pdfinfo did not survive huge stream length"
assert_file_contains "$WORK_DIR/huge-length-info.txt" 'streams=1' "pdfinfo did not count huge-length stream safely"
"${TEST_BIN_DIR}/pdfcheck" "$WORK_DIR/huge-length-stream-ref.pdf" > "$WORK_DIR/huge-length-check.txt"
assert_file_contains "$WORK_DIR/huge-length-check.txt" 'ok: structure checks passed' "pdfcheck treated stream bytes as dangling references"

cat > "$WORK_DIR/encrypted-trailer.pdf" <<'EOF'
%PDF-1.7
1 0 obj
<< /Type /Catalog /Pages 2 0 R >>
endobj
2 0 obj
<< /Type /Pages /Kids [3 0 R] /Count 1 >>
endobj
3 0 obj
<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>
endobj
4 0 obj
<< /Filter /Standard /V 1 /R 2 /O () /U () /P -4 >>
endobj
trailer
<< /Root 1 0 R /Encrypt 4 0 R /Size 5 >>
startxref
0
%%EOF
EOF

"${TEST_BIN_DIR}/pdfinfo" "$WORK_DIR/encrypted-trailer.pdf" > "$WORK_DIR/encrypted-info.txt"
assert_file_contains "$WORK_DIR/encrypted-info.txt" 'encrypted: yes' "pdfinfo did not detect trailer /Encrypt"
if "${TEST_BIN_DIR}/pdfcheck" "$WORK_DIR/encrypted-trailer.pdf" > "$WORK_DIR/encrypted-check.txt" 2>&1; then
    fail "pdfcheck should reject encrypted PDFs"
fi
assert_file_contains "$WORK_DIR/encrypted-check.txt" 'error: encrypted PDFs are unsupported' "pdfcheck did not report encrypted PDF"
if "${TEST_BIN_DIR}/pdfjoin" -o "$WORK_DIR/joined.pdf" "$WORK_DIR/encrypted-trailer.pdf" "$WORK_DIR/huge-length-stream-ref.pdf" > "$WORK_DIR/pdfjoin-encrypted.txt" 2>&1; then
    fail "pdfjoin should reject encrypted PDFs"
fi
assert_file_contains "$WORK_DIR/pdfjoin-encrypted.txt" 'encrypted PDF is not supported\|unsupported or unreadable PDF' "pdfjoin did not reject encrypted PDF"
if "${TEST_BIN_DIR}/pdfsplit" --pages 1 -o "$WORK_DIR/split.pdf" "$WORK_DIR/encrypted-trailer.pdf" > "$WORK_DIR/pdfsplit-encrypted.txt" 2>&1; then
    fail "pdfsplit should reject encrypted PDFs"
fi
assert_file_contains "$WORK_DIR/pdfsplit-encrypted.txt" 'encrypted PDF is not supported\|unsupported or unreadable PDF' "pdfsplit did not reject encrypted PDF"

if ! command -v python3 >/dev/null 2>&1; then
    note "python3 not available; skipping generated PDF compression hardening fixtures"
    exit 0
fi

python3 - <<'PY' "$WORK_DIR"
import os
import sys
import zlib

work = sys.argv[1]

def obj(number, body):
    if not body.endswith(b"\n"):
        body += b"\n"
    return b"%d 0 obj\n" % number + body + b"endobj\n"

def stream_obj(number, dict_body, payload):
    body = dict_body + b"\nstream\n" + payload + b"\nendstream\n"
    return obj(number, body)

def write_pdf(name, objects):
    data = bytearray(b"%PDF-1.7\n")
    for part in objects:
        data.extend(part)
    data.extend(b"trailer\n<< /Root 1 0 R /Size 8 >>\nstartxref\n0\n%%EOF\n")
    with open(os.path.join(work, name), "wb") as handle:
        handle.write(data)

base = [
    obj(1, b"<< /Type /Catalog /Pages 2 0 R >>"),
    obj(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
]

invalid_payload = b"notzlib!"
write_pdf("invalid-flate.pdf", base + [
    obj(3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>"),
    stream_obj(4, b"<< /Length %d /Filter /FlateDecode >>" % len(invalid_payload), invalid_payload),
])

bad_predictor_payload = zlib.compress(b"\x05A")
write_pdf("bad-predictor.pdf", base + [
    obj(3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>"),
    stream_obj(4, b"<< /Length %d /Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 1 >> >>" % len(bad_predictor_payload), bad_predictor_payload),
])

object_stream_body = b"9 0 10 9999 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\n"
first = len(b"9 0 10 9999 ")
object_stream_payload = zlib.compress(object_stream_body)
write_pdf("bad-object-stream.pdf", base + [
    obj(3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>"),
    stream_obj(5, b"<< /Type /ObjStm /N 2 /First %d /Length %d /Filter /FlateDecode >>" % (first, len(object_stream_payload)), object_stream_payload),
])
PY

"${TEST_BIN_DIR}/pdfinfo" --details "$WORK_DIR/invalid-flate.pdf" > "$WORK_DIR/invalid-flate-info.txt"
assert_file_contains "$WORK_DIR/invalid-flate-info.txt" 'filtered_streams: 1' "pdfinfo did not safely count malformed Flate stream"
assert_file_contains "$WORK_DIR/invalid-flate-info.txt" 'text_objects: 0' "pdfinfo decoded malformed Flate bytes as content"
if "${TEST_BIN_DIR}/pdfextract" --stream 4 --decoded "$WORK_DIR/invalid-flate.pdf" > "$WORK_DIR/invalid-flate-extract.bin" 2> "$WORK_DIR/invalid-flate-extract.err"; then
    fail "pdfextract should reject malformed Flate stream"
fi
assert_file_contains "$WORK_DIR/invalid-flate-extract.err" 'could not extract stream' "pdfextract did not clearly reject malformed Flate stream"

"${TEST_BIN_DIR}/pdfinfo" --details "$WORK_DIR/bad-predictor.pdf" > "$WORK_DIR/bad-predictor-info.txt"
assert_file_contains "$WORK_DIR/bad-predictor-info.txt" 'filtered_streams: 1' "pdfinfo did not safely count bad predictor stream"
if "${TEST_BIN_DIR}/pdfextract" --stream 4 --decoded "$WORK_DIR/bad-predictor.pdf" > "$WORK_DIR/bad-predictor-extract.bin" 2> "$WORK_DIR/bad-predictor-extract.err"; then
    fail "pdfextract should reject bad PNG predictor data"
fi
assert_file_contains "$WORK_DIR/bad-predictor-extract.err" 'could not extract stream' "pdfextract did not clearly reject bad predictor data"

"${TEST_BIN_DIR}/pdfinfo" --details "$WORK_DIR/bad-object-stream.pdf" > "$WORK_DIR/bad-object-stream-details.txt"
assert_file_contains "$WORK_DIR/bad-object-stream-details.txt" 'object_streams: 1' "pdfinfo did not count malformed object stream safely"
"${TEST_BIN_DIR}/pdfinfo" --objects "$WORK_DIR/bad-object-stream.pdf" > "$WORK_DIR/bad-object-stream-objects.txt"
if grep -q 'obj 9 0' "$WORK_DIR/bad-object-stream-objects.txt"; then
    fail "pdfinfo exposed an object from a malformed object-stream index"
fi
