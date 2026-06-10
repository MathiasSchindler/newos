#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup pdfextract

if ! command -v python3 >/dev/null 2>&1; then
    note "python3 not available; skipping pdfextract FlateDecode fixture"
    exit 0
fi

python3 - <<'PY' "$WORK_DIR/extract.pdf"
import sys
import zlib

path = sys.argv[1]
raw = b"BT /F1 12 Tf 72 720 Td (Raw PDF) Tj ET\n"
decoded = b"BT /F1 12 Tf 72 700 Td (Decoded PDF) Tj ET\n"
compressed = zlib.compress(decoded)
objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents [5 0 R 6 0 R] >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    b"<< /Length %d >>\nstream\n" % len(raw) + raw + b"endstream",
    b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(compressed) + compressed + b"\nendstream",
    b"<< /Title (Extract Fixture) /Author (NewOS Tests) >>",
]
pdf = bytearray(b"%PDF-1.7\n")
for number, body in enumerate(objects, 1):
    pdf.extend(f"{number} 0 obj\n".encode("ascii"))
    pdf.extend(body)
    if not body.endswith(b"\n"):
        pdf.extend(b"\n")
    pdf.extend(b"endobj\n")
pdf.extend(b"xref\n0 8\n0000000000 65535 f \ntrailer\n<< /Root 1 0 R /Info 7 0 R /Size 8 >>\nstartxref\n0\n%%EOF\n")
with open(path, "wb") as handle:
    handle.write(pdf)
PY

"${TEST_BIN_DIR}/pdfextract" --stream 5 --raw "$WORK_DIR/extract.pdf" > "$WORK_DIR/raw.txt"
assert_file_contains "$WORK_DIR/raw.txt" 'Raw PDF' "pdfextract --raw did not copy unfiltered stream"

"${TEST_BIN_DIR}/pdfextract" --stream 6 --decoded "$WORK_DIR/extract.pdf" > "$WORK_DIR/decoded.txt"
assert_file_contains "$WORK_DIR/decoded.txt" 'Decoded PDF' "pdfextract --decoded did not inflate FlateDecode stream"

"${TEST_BIN_DIR}/pdfextract" --metadata "$WORK_DIR/extract.pdf" > "$WORK_DIR/metadata.txt"
assert_file_contains "$WORK_DIR/metadata.txt" 'title: Extract Fixture' "pdfextract did not print title metadata"
assert_file_contains "$WORK_DIR/metadata.txt" 'author: NewOS Tests' "pdfextract did not print author metadata"

"${TEST_BIN_DIR}/pdfextract" --list-streams "$WORK_DIR/extract.pdf" > "$WORK_DIR/streams.txt"
assert_file_contains "$WORK_DIR/streams.txt" 'object generation raw raw_size decoded decoded_size filter type subtype' "pdfextract --list-streams did not print a header"
assert_file_contains "$WORK_DIR/streams.txt" '5 0 raw=yes' "pdfextract --list-streams did not list the raw stream"
assert_file_contains "$WORK_DIR/streams.txt" 'filter=none' "pdfextract --list-streams did not report unfiltered streams"
assert_file_contains "$WORK_DIR/streams.txt" '6 0 raw=yes' "pdfextract --list-streams did not list the compressed stream"
assert_file_contains "$WORK_DIR/streams.txt" 'filter=FlateDecode' "pdfextract --list-streams did not report FlateDecode"
assert_file_contains "$WORK_DIR/streams.txt" 'decoded_size=unknown' "pdfextract --list-streams should not inflate just to size compressed streams"
