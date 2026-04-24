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

assert_command_succeeds "$ROOT_DIR/build/od" -A x -t x1 -j 1 -N 4 "$WORK_DIR/text.txt" > "$WORK_DIR/od_hex.out"
assert_file_contains "$WORK_DIR/od_hex.out" '^0000001 6c 70 68 61$' "od -A/-t/-j/-N did not dump the selected bytes as hex"

assert_command_succeeds "$ROOT_DIR/build/od" -A n -t c -N 2 "$WORK_DIR/text.txt" > "$WORK_DIR/od_char.out"
assert_file_contains "$WORK_DIR/od_char.out" '^  a   l$' "od -A n -t c did not render printable characters"

assert_command_succeeds "$ROOT_DIR/build/hexdump" "$WORK_DIR/text.txt" > "$WORK_DIR/hexdump.out"
assert_file_contains "$WORK_DIR/hexdump.out" '61 6c 70 68 61' "hexdump did not include the expected byte sequence"

assert_command_succeeds "$ROOT_DIR/build/hexdump" -C -s 6 -n 4 "$WORK_DIR/text.txt" > "$WORK_DIR/hexdump_slice.out"
assert_file_contains "$WORK_DIR/hexdump_slice.out" '^00000006  62 65 74 61' "hexdump -C/-s/-n did not dump the selected slice"
assert_file_contains "$WORK_DIR/hexdump_slice.out" '^0000000a$' "hexdump -n did not report the expected final offset"

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
