#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir strings)

note "phase1 text: strings"

printf '\001\002HELLO123\000bye\000' > "$WORK_DIR/input.bin"
"$ROOT_DIR/build/strings" "$WORK_DIR/input.bin" > "$WORK_DIR/out.txt"

assert_file_contains "$WORK_DIR/out.txt" '^HELLO123$' "strings did not extract the printable payload"
if grep '^bye$' "$WORK_DIR/out.txt" >/dev/null 2>&1; then
    fail "strings should not emit short sequences below the minimum length"
fi

"$ROOT_DIR/build/strings" -a "$WORK_DIR/input.bin" > "$WORK_DIR/all.out"
assert_file_contains "$WORK_DIR/all.out" '^HELLO123$' "strings -a did not scan the full byte stream"

"$ROOT_DIR/build/strings" -t x "$WORK_DIR/input.bin" > "$WORK_DIR/offset.out"
assert_file_contains "$WORK_DIR/offset.out" '^[0-9a-f][0-9a-f]* HELLO123$' "strings -t x did not include the expected offset"

printf 'W\000i\000d\000e\000\000\000' > "$WORK_DIR/wide_le.bin"
"$ROOT_DIR/build/strings" -e l -n 4 "$WORK_DIR/wide_le.bin" > "$WORK_DIR/wide_le.out"
assert_file_contains "$WORK_DIR/wide_le.out" '^Wide$' "strings -e l did not decode UTF-16LE-style text"

cat > "$WORK_DIR/object_marker.c" <<'EOF'
const char object_marker[] = "OBJECT_SECTION_MARKER";
int main(void) {
    return object_marker[0] == 'O' ? 0 : 1;
}
EOF
cc -o "$WORK_DIR/object_marker" "$WORK_DIR/object_marker.c"
"$ROOT_DIR/build/strings" -d "$WORK_DIR/object_marker" > "$WORK_DIR/object_sections.out"
assert_file_contains "$WORK_DIR/object_sections.out" '^OBJECT_SECTION_MARKER$' "strings -d did not scan host object sections"

{
    printf 'MZ'
    dd if=/dev/zero bs=1 count=58 2>/dev/null
    printf '\200\000\000\000'
    dd if=/dev/zero bs=1 count=64 2>/dev/null
    printf 'PE\000\000'
    printf '\144\206'
    printf '\001\000'
    dd if=/dev/zero bs=1 count=12 2>/dev/null
    printf '\110\000'
    printf '\042\000'
    printf '\013\002'
    dd if=/dev/zero bs=1 count=14 2>/dev/null
    printf '\000\020\000\000'
    dd if=/dev/zero bs=1 count=44 2>/dev/null
    printf '\003\000'
    printf '\140\201'
    dd if=/dev/zero bs=1 count=4 2>/dev/null
    printf '.rdata\000\000'
    printf '\040\000\000\000'
    printf '\000\020\000\000'
    printf '\040\000\000\000'
    printf '\000\002\000\000'
    dd if=/dev/zero bs=1 count=12 2>/dev/null
    printf '\100\000\000\100'
    dd if=/dev/zero bs=1 count=248 2>/dev/null
    printf 'PE_SECTION_MARKER\000more-section-data'
} > "$WORK_DIR/pe_strings.exe"
"$ROOT_DIR/build/strings" -d "$WORK_DIR/pe_strings.exe" > "$WORK_DIR/pe_sections.out"
assert_file_contains "$WORK_DIR/pe_sections.out" '^PE_SECTION_MARKER$' "strings -d did not scan PE/COFF sections"
