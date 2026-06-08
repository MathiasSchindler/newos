#!/bin/sh
set -eu

hashclash_dir=${HASHCLASH_DIR:-${1:-}}
out_dir=${OUT_DIR:-out}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ -z "$hashclash_dir" ]; then
    echo "hashclash-elf-demo: set HASHCLASH_DIR or pass the HashClash directory" >&2
    exit 1
fi
fastcoll=$hashclash_dir/bin/md5_fastcoll
if [ ! -x "$fastcoll" ]; then
    echo "hashclash-elf-demo: cannot find executable md5_fastcoll under $hashclash_dir" >&2
    exit 1
fi

tmp_base=${TMPDIR:-/tmp}
work_dir=$(mktemp -d "$tmp_base/newos-md5-elf-ipc.XXXXXX")
mkdir -p "$out_dir"

cleanup() {
    rm -rf "$work_dir"
}
trap cleanup EXIT INT TERM

python3 - <<'PY' "$work_dir/prefix.bin"
import struct
import sys

path = sys.argv[1]
prefix_size = 128
base = 0x400000
code_offset = 8192
file_size = 12288

data = bytearray(prefix_size)
data[0:4] = b'\x7fELF'
data[4] = 2
data[5] = 1
data[6] = 1
struct.pack_into('<HHIQQQIHHHHHH', data, 16, 2, 62, 1, base + code_offset, 64, 0, 0, 64, 56, 1, 0, 0, 0)
struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, base, base, file_size, file_size, 0x1000)
with open(path, 'wb') as file:
    file.write(data)
PY

(
    cd "$work_dir"
    "$fastcoll" -q -p prefix.bin -o collision1.bin collision2.bin
)

python3 - <<'PY' "$work_dir/collision1.bin" "$work_dir/collision2.bin" "$out_dir/elf-true" "$out_dir/elf-false"
import hashlib
import os
import struct
import sys

collision1_path, collision2_path, true_path, false_path = sys.argv[1:]
prefix_size = 128
base = 0x400000
code_offset = 8192
file_size = 12288

with open(collision1_path, 'rb') as file:
    collision1 = file.read()
with open(collision2_path, 'rb') as file:
    collision2 = file.read()

if len(collision1) != len(collision2):
    raise SystemExit('hashclash-elf-demo: collision lengths differ')
if len(collision1) <= prefix_size:
    raise SystemExit('hashclash-elf-demo: collision output did not extend the prefix')
if len(collision1) >= code_offset:
    raise SystemExit('hashclash-elf-demo: collision payload is too large for the reserved code offset')
if hashlib.md5(collision1).digest() != hashlib.md5(collision2).digest():
    raise SystemExit('hashclash-elf-demo: HashClash outputs do not collide')

chosen = None
for offset, (left, right) in enumerate(zip(collision1, collision2)):
    if offset < prefix_size or left == right:
        continue
    diff = left ^ right
    for bit in range(8):
        if diff & (1 << bit):
            left_bit = (left >> bit) & 1
            right_bit = (right >> bit) & 1
            invert = left_bit
            if (left_bit ^ invert) == 0 and (right_bit ^ invert) == 1:
                chosen = (offset, bit, invert)
                break
    if chosen is not None:
        break

if chosen is None:
    raise SystemExit('hashclash-elf-demo: no usable differing collision bit found')

offset, bit, invert = chosen
entry = base + code_offset
target = base + offset
rip_after_mov = entry + 6
displacement = target - rip_after_mov
if not -(1 << 31) <= displacement < (1 << 31):
    raise SystemExit('hashclash-elf-demo: selected byte is outside RIP-relative range')

code = bytearray()
code += b'\x0f\xb6\x3d' + struct.pack('<i', displacement)
code += b'\xc1\xef' + bytes([bit])
code += b'\x83\xe7\x01'
if invert:
    code += b'\x83\xf7\x01'
code += b'\xb8\x3c\x00\x00\x00'
code += b'\x0f\x05'

def finish(image):
    if len(image) > code_offset:
        raise SystemExit('hashclash-elf-demo: image overlaps code offset')
    result = bytearray(image)
    result += b'\0' * (code_offset - len(result))
    result += code
    result += b'\0' * (file_size - len(result))
    return bytes(result)

true_image = finish(collision1)
false_image = finish(collision2)

if hashlib.md5(true_image).digest() != hashlib.md5(false_image).digest():
    raise SystemExit('hashclash-elf-demo: final ELF files do not collide')
if true_image == false_image:
    raise SystemExit('hashclash-elf-demo: final ELF files are identical')

for path, image in ((true_path, true_image), (false_path, false_image)):
    with open(path, 'wb') as file:
        file.write(image)
    os.chmod(path, 0o755)

digest = hashlib.md5(true_image).hexdigest()
print(f'wrote {true_path}')
print(f'wrote {false_path}')
print(f'same md5: {digest}')
print('different files: yes')
print(f'behavior bit: file offset {offset}, bit {bit}, invert {invert}')
print(f'sizes: {len(true_image)} bytes, {len(false_image)} bytes')
PY

sh "$script_dir/verify.sh" "$out_dir/elf-true" "$out_dir/elf-false"