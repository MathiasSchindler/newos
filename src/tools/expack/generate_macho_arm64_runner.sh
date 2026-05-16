#!/bin/sh
set -eu

codec_name=$1
codec_value=$2
output_file=$3

work_dir=${TMPDIR:-/tmp}/expack-macho-runner-$$
trap 'rm -rf "$work_dir"' EXIT INT HUP TERM
mkdir -p "$work_dir"

cc=${CC:-clang}
sdk=${MACOSX_SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
template_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
template_file=$template_dir/macho_arm64_runner_template.c
runner_file=$work_dir/runner
blob_file=$work_dir/runner.text

"$cc" -target arm64-apple-macos11 -isysroot "$sdk" -Oz -ffreestanding -fno-stack-protector \
    -fno-unwind-tables -fno-asynchronous-unwind-tables -DEXPACK_MACHO_RUNNER_CODEC="$codec_value" \
    -nodefaultlibs -lSystem -Wl,-e,_expack_runner_entry -Wl,-dead_strip -Wl,-no_uuid \
    "$template_file" -o "$runner_file"

set -- $(otool -l "$runner_file" | awk '
    $1 == "sectname" && $2 == "__text" { in_text = 1 }
    in_text && $1 == "offset" { offset = $2 }
    in_text && $1 == "size" { size = $2 }
    in_text && $1 == "reserved2" { print offset, size; exit }
')
offset=$1
size=$2
dd if="$runner_file" of="$blob_file" bs=1 skip="$offset" count="$size" 2>/dev/null

python3 - "$codec_name" "$blob_file" > "$output_file" <<'PY'
import sys

codec_name = sys.argv[1]
path = sys.argv[2]
data = open(path, "rb").read()
markers = {
    "payload delta": bytes.fromhex("8877665544332211"),
    "payload size": bytes.fromhex("1122334455667788"),
    "original size": bytes.fromhex("0807060504030201"),
}
for label, marker in markers.items():
    if data.find(marker) < 0:
        raise SystemExit(f"missing {label} marker in {codec_name} runner")
print(f"/* Generated from macho_arm64_runner_template.c for {codec_name}. */")
for offset in range(0, len(data), 12):
    chunk = data[offset:offset + 12]
    print("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ("," if offset + 12 < len(data) else ""))
PY