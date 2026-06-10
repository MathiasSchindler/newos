#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup pdf_modern_writer

if ! command -v python3 >/dev/null 2>&1; then
    note "python3 not available; skipping PDF modern writer fixtures"
    exit 0
fi

python3 - <<'PY' "$WORK_DIR/plain.pdf" "$WORK_DIR/xref-stream.pdf" "$WORK_DIR/object-stream.pdf" "$WORK_DIR/bad-object-stream.pdf"
import struct
import sys
import zlib

plain_path, xref_path, object_path, bad_path = sys.argv[1:]

def add_object(pdf, offsets, number, body):
    offsets[number] = len(pdf)
    pdf.extend(f"{number} 0 obj\n".encode("ascii"))
    pdf.extend(body)
    if not body.endswith(b"\n"):
        pdf.extend(b"\n")
    pdf.extend(b"endobj\n")

def add_content(pdf, offsets, number, text, compressed=False):
    content = f"BT /F1 12 Tf 72 720 Td ({text}) Tj ET\n".encode("ascii")
    if compressed:
        payload = zlib.compress(content)
        add_object(pdf, offsets, number, f"<< /Length {len(payload)} /Filter /FlateDecode >>\nstream\n".encode("ascii") + payload + b"\nendstream")
    else:
        add_object(pdf, offsets, number, f"<< /Length {len(content)} >>\nstream\n".encode("ascii") + content + b"endstream")

def write_plain(path):
    pdf = bytearray(b"%PDF-1.7\n")
    offsets = {}
    add_object(pdf, offsets, 1, b"<< /Type /Catalog /Pages 2 0 R >>")
    add_object(pdf, offsets, 2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    add_object(pdf, offsets, 3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    add_object(pdf, offsets, 4, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
    add_content(pdf, offsets, 5, "Plain", compressed=False)
    xref = len(pdf)
    pdf.extend(b"xref\n0 6\n0000000000 65535 f \n")
    for number in range(1, 6):
        pdf.extend(f"{offsets[number]:010d} 00000 n \n".encode("ascii"))
    pdf.extend(f"trailer\n<< /Root 1 0 R /Size 6 >>\nstartxref\n{xref}\n%%EOF\n".encode("ascii"))
    open(path, "wb").write(pdf)

def write_xref_stream(path):
    pdf = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}
    add_object(pdf, offsets, 1, b"<< /Type /Catalog /Pages 2 0 R >>")
    add_object(pdf, offsets, 2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    add_object(pdf, offsets, 3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    add_object(pdf, offsets, 4, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
    add_content(pdf, offsets, 5, "XRef stream", compressed=True)
    xref_offset = len(pdf)
    entries = [(0, 0, 65535)] + [(1, offsets[number], 0) for number in range(1, 6)] + [(1, xref_offset, 0)]
    raw = bytearray()
    for entry_type, field1, field2 in entries:
        raw.append(entry_type)
        raw.extend(struct.pack(">I", field1))
        raw.extend(struct.pack(">H", field2))
    payload = zlib.compress(bytes(raw))
    pdf.extend(b"6 0 obj\n")
    pdf.extend(f"<< /Type /XRef /Size 7 /Root 1 0 R /W [1 4 2] /Length {len(payload)} /Filter /FlateDecode >>\nstream\n".encode("ascii"))
    pdf.extend(payload)
    pdf.extend(b"\nendstream\nendobj\n")
    pdf.extend(f"startxref\n{xref_offset}\n%%EOF\n".encode("ascii"))
    open(path, "wb").write(pdf)

def object_stream(entries):
    offsets = []
    body = bytearray()
    for number, data in entries:
        offsets.append((number, len(body)))
        body.extend(data)
        body.extend(b"\n")
    header = b" ".join(f"{number} {offset}".encode("ascii") for number, offset in offsets) + b" "
    return header, len(header), bytes(body)

def write_object_stream(path):
    pdf = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}
    add_object(pdf, offsets, 1, b"<< /Type /Catalog /Pages 3 0 R >>")
    entries = [
        (3, b"<< /Type /Pages /Kids [4 0 R] /Count 1 >>"),
        (4, b"<< /Type /Page /Parent 3 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 6 0 R >>"),
        (5, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"),
    ]
    header, first, bodies = object_stream(entries)
    payload = zlib.compress(header + bodies)
    add_object(pdf, offsets, 2, f"<< /Type /ObjStm /N 3 /First {first} /Length {len(payload)} /Filter /FlateDecode >>\nstream\n".encode("ascii") + payload + b"\nendstream")
    add_content(pdf, offsets, 6, "Object stream", compressed=True)
    xref_offset = len(pdf)
    xref_entries = [
        (0, 0, 65535),
        (1, offsets[1], 0),
        (1, offsets[2], 0),
        (2, 2, 0),
        (2, 2, 1),
        (2, 2, 2),
        (1, offsets[6], 0),
        (1, xref_offset, 0),
    ]
    raw = bytearray()
    for entry_type, field1, field2 in xref_entries:
        raw.append(entry_type)
        raw.extend(struct.pack(">I", field1))
        raw.extend(struct.pack(">H", field2))
    xref_payload = zlib.compress(bytes(raw))
    pdf.extend(b"7 0 obj\n")
    pdf.extend(f"<< /Type /XRef /Size 8 /Root 1 0 R /W [1 4 2] /Length {len(xref_payload)} /Filter /FlateDecode >>\nstream\n".encode("ascii"))
    pdf.extend(xref_payload)
    pdf.extend(b"\nendstream\nendobj\n")
    pdf.extend(f"startxref\n{xref_offset}\n%%EOF\n".encode("ascii"))
    open(path, "wb").write(pdf)

def write_bad_object_stream(path):
    pdf = bytearray(b"%PDF-1.7\n")
    offsets = {}
    add_object(pdf, offsets, 1, b"<< /Type /Catalog /Pages 2 0 R >>")
    add_object(pdf, offsets, 2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    add_object(pdf, offsets, 3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>")
    add_object(pdf, offsets, 4, b"<< /Type /ObjStm /N 1 /First 4 /Length 2 /Filter /ASCIIHexDecode >>\nstream\n00\nendstream")
    xref = len(pdf)
    pdf.extend(b"xref\n0 5\n0000000000 65535 f \n")
    for number in range(1, 5):
        pdf.extend(f"{offsets[number]:010d} 00000 n \n".encode("ascii"))
    pdf.extend(f"trailer\n<< /Root 1 0 R /Size 5 >>\nstartxref\n{xref}\n%%EOF\n".encode("ascii"))
    open(path, "wb").write(pdf)

write_plain(plain_path)
write_xref_stream(xref_path)
write_object_stream(object_path)
write_bad_object_stream(bad_path)
PY

"${TEST_BIN_DIR}/pdfsplit" --pages 1 -o "$WORK_DIR/xref-split.pdf" "$WORK_DIR/xref-stream.pdf"
"${TEST_BIN_DIR}/pdfinfo" "$WORK_DIR/xref-split.pdf" > "$WORK_DIR/xref-split.info"
assert_file_contains "$WORK_DIR/xref-split.info" 'pages: 1' "pdfsplit rejected xref-stream input"

"${TEST_BIN_DIR}/pdfjoin" -o "$WORK_DIR/xref-joined.pdf" "$WORK_DIR/xref-stream.pdf" "$WORK_DIR/plain.pdf"
"${TEST_BIN_DIR}/pdfinfo" "$WORK_DIR/xref-joined.pdf" > "$WORK_DIR/xref-joined.info"
assert_file_contains "$WORK_DIR/xref-joined.info" 'pages: 2' "pdfjoin rejected xref-stream input"

"${TEST_BIN_DIR}/pdfsplit" --pages 1 -o "$WORK_DIR/object-split.pdf" "$WORK_DIR/object-stream.pdf"
"${TEST_BIN_DIR}/pdfinfo" --details "$WORK_DIR/object-split.pdf" > "$WORK_DIR/object-split.info"
assert_file_contains "$WORK_DIR/object-split.info" 'pages: 1' "pdfsplit did not materialize object-stream page"
assert_file_contains "$WORK_DIR/object-split.info" 'fonts: 1' "pdfsplit did not materialize object-stream font"

"${TEST_BIN_DIR}/pdfjoin" -o "$WORK_DIR/object-joined.pdf" "$WORK_DIR/object-stream.pdf" "$WORK_DIR/plain.pdf"
"${TEST_BIN_DIR}/pdfinfo" "$WORK_DIR/object-joined.pdf" > "$WORK_DIR/object-joined.info"
assert_file_contains "$WORK_DIR/object-joined.info" 'pages: 2' "pdfjoin did not materialize object-stream input"

set +e
"${TEST_BIN_DIR}/pdfsplit" --pages 1 -o "$WORK_DIR/bad-split.pdf" "$WORK_DIR/bad-object-stream.pdf" 2> "$WORK_DIR/bad.err"
bad_status=$?
set -e
assert_exit_code "$bad_status" 1 "pdfsplit accepted an unsupported object stream"
assert_file_contains "$WORK_DIR/bad.err" 'unsupported compressed object stream in PDF' "pdfsplit object-stream rejection was not specific"
