#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup pdfgrep

if ! command -v python3 >/dev/null 2>&1; then
    note "python3 not available; skipping pdfgrep FlateDecode fixture"
    exit 0
fi

python3 - <<'PY' "$WORK_DIR/grep.pdf"
import sys
import zlib

path = sys.argv[1]
plain = b"BT /F1 12 Tf 72 720 Td (Plain Visible) Tj ET\n"
flate = zlib.compress(b"BT /F1 12 Tf 72 700 Td [(Decoded) 20 ( Visible)] TJ ET\n")
objects = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> /XObject << /BadImage 7 0 R >> >> /Contents [5 0 R 6 0 R] >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    b"<< /Length %d >>\nstream\n" % len(plain) + plain + b"endstream",
    b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(flate) + flate + b"\nendstream",
    b"<< /Type /XObject /Subtype /Image /Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace /DeviceGray /Length 8 /Filter /FlateDecode >>\nstream\nnotzlib!\nendstream",
]
pdf = bytearray(b"%PDF-1.7\n")
for number, body in enumerate(objects, 1):
    pdf.extend(f"{number} 0 obj\n".encode("ascii"))
    pdf.extend(body)
    if not body.endswith(b"\n"):
        pdf.extend(b"\n")
    pdf.extend(b"endobj\n")
pdf.extend(b"trailer\n<< /Root 1 0 R /Size 7 >>\n%%EOF\n")
with open(path, "wb") as handle:
    handle.write(pdf)
PY

"${TEST_BIN_DIR}/pdfgrep" 'Plain Visible' "$WORK_DIR/grep.pdf" > "$WORK_DIR/plain.txt"
assert_file_contains "$WORK_DIR/plain.txt" 'Plain Visible' "pdfgrep did not find uncompressed visible text"
assert_file_contains "$WORK_DIR/plain.txt" '5:Plain Visible' "pdfgrep did not include the matching stream object number"

"${TEST_BIN_DIR}/pdfgrep" 'Decoded Visible' "$WORK_DIR/grep.pdf" > "$WORK_DIR/decoded.txt"
assert_file_contains "$WORK_DIR/decoded.txt" 'Decoded Visible' "pdfgrep did not find decoded FlateDecode visible text"

"${TEST_BIN_DIR}/pdfgrep" -n -C 2 'Visible' "$WORK_DIR/grep.pdf" > "$WORK_DIR/context.txt"
assert_file_contains "$WORK_DIR/context.txt" '5:...n Visible' "pdfgrep context output did not include object number and clipped context"

if "${TEST_BIN_DIR}/pdfgrep" 'Not Present' "$WORK_DIR/grep.pdf" > "$WORK_DIR/no-match.txt" 2>&1; then
    fail "pdfgrep should return nonzero when no match is found"
fi
assert_text_equals "$(cat "$WORK_DIR/no-match.txt")" "" "pdfgrep no-match should not print output"
