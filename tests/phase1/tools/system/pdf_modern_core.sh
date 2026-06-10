#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup pdf_modern_core

if ! command -v python3 >/dev/null 2>&1; then
    note "python3 not available; skipping PDF xref/object stream fixture"
    exit 0
fi

python3 - <<'PY' "$WORK_DIR/modern.pdf"
import struct
import sys
import zlib

out_path = sys.argv[1]
pdf = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
offsets = {}

def add_object(number, body):
    offsets[number] = len(pdf)
    pdf.extend(f"{number} 0 obj\n".encode("ascii"))
    pdf.extend(body)
    if not body.endswith(b"\n"):
        pdf.extend(b"\n")
    pdf.extend(b"endobj\n")

def object_stream(entries):
    offsets_only = []
    body = bytearray()
    for number, data in entries:
        offsets_only.append((number, len(body)))
        body.extend(data)
        body.extend(b"\n")
    header = b""
    while True:
        header = b" ".join(f"{number} {offset}".encode("ascii") for number, offset in offsets_only) + b" "
        first = len(header)
        new_offsets = []
        body = bytearray()
        for number, data in entries:
            new_offsets.append((number, len(body)))
            body.extend(data)
            body.extend(b"\n")
        if new_offsets == offsets_only:
            return header, first, bytes(body)
        offsets_only = new_offsets

add_object(1, b"<< /Type /Catalog /Pages 3 0 R >>")

obj_entries = [
    (3, b"<< /Type /Pages /Kids [4 0 R] /Count 1 >>"),
    (4, b"<< /Type /Page /Parent 3 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 6 0 R >>"),
    (5, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"),
]
header, first, bodies = object_stream(obj_entries)
obj_payload = zlib.compress(header + bodies)
add_object(2, f"<< /Type /ObjStm /N 3 /First {first} /Length {len(obj_payload)} /Filter /FlateDecode >>\nstream\n".encode("ascii") + obj_payload + b"\nendstream")

content = zlib.compress(b"BT /F1 12 Tf 72 720 Td (Modern PDF) Tj ET\n")
add_object(6, f"<< /Length {len(content)} /Filter /FlateDecode >>\nstream\n".encode("ascii") + content + b"\nendstream")

xref_offset = len(pdf)
entries = [
    (0, 0, 65535),
    (1, offsets[1], 0),
    (1, offsets[2], 0),
    (2, 2, 0),
    (2, 2, 1),
    (2, 2, 2),
    (1, offsets[6], 0),
    (1, xref_offset, 0),
    (2, 2, 7),
]
xref_raw = bytearray()
for entry_type, field1, field2 in entries:
    xref_raw.append(0)
    xref_raw.append(entry_type)
    xref_raw.extend(struct.pack(">I", field1))
    xref_raw.extend(struct.pack(">H", field2))
xref_payload = zlib.compress(bytes(xref_raw))
pdf.extend(b"7 0 obj\n")
pdf.extend(f"<< /Type /XRef /Size 9 /Root 1 0 R /W [1 4 2] /Index [0 9] /DecodeParms << /Predictor 12 /Columns 7 >> /Length {len(xref_payload)} /Filter /FlateDecode >>\nstream\n".encode("ascii"))
pdf.extend(xref_payload)
pdf.extend(b"\nendstream\nendobj\n")
pdf.extend(f"startxref\n{xref_offset}\n%%EOF\n".encode("ascii"))

with open(out_path, "wb") as handle:
    handle.write(pdf)
PY

"${TEST_BIN_DIR}/pdfinfo" --plain "$WORK_DIR/modern.pdf" > "$WORK_DIR/plain.txt"
assert_file_contains "$WORK_DIR/plain.txt" 'pages=1' "pdfinfo did not count page from object stream"
assert_file_contains "$WORK_DIR/plain.txt" 'objects=8' "pdfinfo did not count xref/object stream objects"
assert_file_contains "$WORK_DIR/plain.txt" 'streams=3' "pdfinfo did not count modern PDF streams"
assert_file_contains "$WORK_DIR/plain.txt" 'fonts=1' "pdfinfo did not find font dictionary in object stream"

"${TEST_BIN_DIR}/pdfinfo" --details "$WORK_DIR/modern.pdf" > "$WORK_DIR/details.txt"
assert_file_contains "$WORK_DIR/details.txt" 'xref_streams: 1' "pdfinfo did not count xref stream"
assert_file_contains "$WORK_DIR/details.txt" 'object_streams: 1' "pdfinfo did not count object stream"
assert_file_contains "$WORK_DIR/details.txt" 'filtered_streams: 3' "pdfinfo did not count FlateDecode streams"
assert_file_contains "$WORK_DIR/details.txt" 'text_objects: 1' "pdfinfo did not scan FlateDecode content stream"
assert_file_contains "$WORK_DIR/details.txt" 'text_show_ops: 1' "pdfinfo did not scan FlateDecode text operator"

"${TEST_BIN_DIR}/pdfinfo" --objects "$WORK_DIR/modern.pdf" > "$WORK_DIR/objects.txt"
assert_file_contains "$WORK_DIR/objects.txt" 'obj 3 0 offset=' "pdfinfo did not expose object-stream pages tree object"
assert_file_contains "$WORK_DIR/objects.txt" 'obj 4 0 offset=' "pdfinfo did not expose object-stream page object"
assert_file_contains "$WORK_DIR/objects.txt" 'obj 5 0 offset=' "pdfinfo did not expose object-stream font object"
assert_file_contains "$WORK_DIR/objects.txt" 'obj 8 0 offset=2 type=- subtype=- stream=no' "pdfinfo did not expose xref-stream-only object"
assert_file_contains "$WORK_DIR/objects.txt" 'type=Font subtype=Type1 stream=no' "pdfinfo did not analyze object-stream font dictionary"
