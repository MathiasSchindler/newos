#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup binary_inspect

printf 'alpha\nbeta\n' > "$WORK_DIR/text.txt"

assert_command_succeeds "$ROOT_DIR/build/file" "$WORK_DIR/text.txt" > "$WORK_DIR/file_text.out"
assert_file_contains "$WORK_DIR/file_text.out" 'ASCII text' "file did not recognize a text file"

assert_command_succeeds "$ROOT_DIR/build/file" -i "$WORK_DIR/text.txt" > "$WORK_DIR/file_mime.out"
assert_file_contains "$WORK_DIR/file_mime.out" 'text/plain' "file -i did not report a text mime type"

assert_command_succeeds "$ROOT_DIR/build/od" "$WORK_DIR/text.txt" > "$WORK_DIR/od.out"
assert_file_contains "$WORK_DIR/od.out" '^0000000 ' "od did not print the expected offset prefix"

assert_command_succeeds "$ROOT_DIR/build/hexdump" "$WORK_DIR/text.txt" > "$WORK_DIR/hexdump.out"
assert_file_contains "$WORK_DIR/hexdump.out" '61 6c 70 68 61' "hexdump did not include the expected byte sequence"

cat > "$WORK_DIR/probe.c" <<'EOF'
int main(void) {
    return 0;
}
EOF

assert_command_succeeds cc -o "$WORK_DIR/probe" "$WORK_DIR/probe.c"

assert_command_succeeds "$ROOT_DIR/build/readelf" -h "$WORK_DIR/probe" > "$WORK_DIR/readelf.out"
if ! grep -q '^ELF Header:' "$WORK_DIR/readelf.out"; then
    assert_file_contains "$WORK_DIR/readelf.out" '^Mach-O Header:' "readelf -h did not print a recognizable object header"
fi
assert_file_contains "$WORK_DIR/readelf.out" 'Machine:' "readelf -h did not include the machine field"

assert_command_succeeds "$ROOT_DIR/build/objdump" -f "$WORK_DIR/probe" > "$WORK_DIR/objdump.out"
if ! grep -q 'file format elf' "$WORK_DIR/objdump.out"; then
    assert_file_contains "$WORK_DIR/objdump.out" 'file format mach-o' "objdump -f did not identify the hosted object format"
fi

assert_command_succeeds "$ROOT_DIR/build/strip" -o "$WORK_DIR/probe.stripped" "$WORK_DIR/probe"
[ -f "$WORK_DIR/probe.stripped" ] || fail "strip did not create the requested output file"
orig_size=$(wc -c < "$WORK_DIR/probe" | tr -d ' ')
stripped_size=$(wc -c < "$WORK_DIR/probe.stripped" | tr -d ' ')
[ "$stripped_size" -le "$orig_size" ] || fail "strip output was unexpectedly larger than the original binary"
