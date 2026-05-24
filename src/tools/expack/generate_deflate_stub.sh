#!/bin/sh
set -eu

output_file=${1:-src/tools/expack/deflate_stub.inc}
build_dir=${BUILD_DIR:-build/expack-deflate-stub}
cc=${CC:-$(command -v clang 2>/dev/null || command -v cc)}
template_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
template_file=$template_dir/deflate_stub_template.c
stub_file=$build_dir/deflate_stub
linker_script=$build_dir/deflate_stub.ld

mkdir -p "$build_dir"
cat > "$linker_script" <<'EOF'
ENTRY(_start)
PHDRS
{
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
}
SECTIONS
{
    . = 0x400000 + SIZEOF_HEADERS;
    .text : SUBALIGN(1) {
        *(.text*)
        *(.rodata*)
        *(.payload*)
    } :text
    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.note.*)
        *(.eh_frame*)
        *(.eh_frame_hdr*)
        *(.gnu*)
        *(.rela*)
        *(.data*)
        *(.bss*)
    }
}
EOF

"$cc" -Oz -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables \
    -fno-asynchronous-unwind-tables -nostdlib -fno-pie -no-pie \
    -Wl,--build-id=none -Wl,-T,"$linker_script" "$template_file" -o "$stub_file"

python3 - "$stub_file" "$output_file" <<'PY'
import pathlib
import struct
import subprocess
import sys

stub_path = pathlib.Path(sys.argv[1])
output_path = pathlib.Path(sys.argv[2])
data = stub_path.read_bytes()
symbols = {}
for line in subprocess.check_output(["nm", "-n", str(stub_path)], text=True).splitlines():
    parts = line.split()
    if len(parts) >= 3:
        symbols[parts[2]] = int(parts[0], 16)
if "payload_start" not in symbols:
    raise SystemExit("missing payload_start symbol")
phoff = struct.unpack_from("<Q", data, 32)[0]
phentsize = struct.unpack_from("<H", data, 54)[0]
phnum = struct.unpack_from("<H", data, 56)[0]
payload_file_offset = None
for index in range(phnum):
    offset = phoff + index * phentsize
    if struct.unpack_from("<I", data, offset)[0] == 1:
        segment_offset = struct.unpack_from("<Q", data, offset + 8)[0]
        segment_vaddr = struct.unpack_from("<Q", data, offset + 16)[0]
        payload_file_offset = segment_offset + symbols["payload_start"] - segment_vaddr
        break
if payload_file_offset is None:
    raise SystemExit("missing load segment")
stub = data[120:payload_file_offset]
payload_marker = bytes.fromhex("1122334455667788")
original_marker = bytes.fromhex("8877665544332211")
payload_offset = stub.find(payload_marker)
original_offset = stub.find(original_marker)
if payload_offset < 0 or original_offset < 0:
    raise SystemExit("missing size patch marker")
lines = ["/* Generated from deflate_stub_template.c. */"]
for offset in range(0, len(stub), 12):
    chunk = stub[offset:offset + 12]
    suffix = "," if offset + 12 < len(stub) else ""
    lines.append("    " + ", ".join(f"0x{byte:02x}U" for byte in chunk) + suffix)
output_path.write_text("\n".join(lines) + "\n", encoding="ascii")
print(f"stub_size={len(stub)}")
print(f"payload_size_offset={payload_offset}")
print(f"original_size_offset={original_offset}")
PY
