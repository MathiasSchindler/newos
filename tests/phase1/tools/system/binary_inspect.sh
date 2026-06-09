#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup binary_inspect

printf 'alpha\nbeta\n' > "$WORK_DIR/text.txt"

assert_command_succeeds "${TEST_BIN_DIR}/file" "$WORK_DIR/text.txt" > "$WORK_DIR/file_text.out"
assert_file_contains "$WORK_DIR/file_text.out" 'ASCII text' "file did not recognize a text file"

{
    printf '\317\372\355\376'
    printf '\014\000\000\001'
    printf '\000\000\000\000'
    printf '\002\000\000\000'
    printf '\000\000\000\000'
    printf '\000\000\000\000'
    printf '\000\000\000\000'
    printf '\000\000\000\000'
} > "$WORK_DIR/minimal_macho"
assert_command_succeeds "${TEST_BIN_DIR}/expack" --analyze "$WORK_DIR/minimal_macho" > "$WORK_DIR/expack_macho.out"
assert_file_contains "$WORK_DIR/expack_macho.out" 'format Mach-O 64-bit arm64' "expack did not analyze a Mach-O executable image"
assert_file_contains "$WORK_DIR/expack_macho.out" '^selected: ' "expack did not select a Mach-O compression candidate"
assert_command_succeeds "${TEST_BIN_DIR}/expack" "$WORK_DIR/minimal_macho" > "$WORK_DIR/expack_macho_container.out"
assert_file_contains "$WORK_DIR/expack_macho_container.out" '^expack: input .*format Mach-O' "expack did not report Mach-O input statistics while packing"
assert_file_contains "$WORK_DIR/expack_macho_container.out" '^  lzss/long-match: payload .* packed ' "expack did not report runnable Mach-O LZSS compression candidates while packing"
assert_file_contains "$WORK_DIR/expack_macho_container.out" '^  lzrep: payload ' "expack did not report runnable Mach-O compression candidates while packing"
assert_file_contains "$WORK_DIR/expack_macho_container.out" 'wrote Mach-O prototype container' "expack did not report Mach-O container output"
assert_file_contains "$WORK_DIR/expack_macho_container.out" 'output .* bytes, payload ' "expack did not report Mach-O output statistics while packing"
"${TEST_BIN_DIR}/file" "$WORK_DIR/minimal_macho-pack" > "$WORK_DIR/file_macho_container.out"
assert_file_contains "$WORK_DIR/file_macho_container.out" 'Mach-O 64-bit executable arm64' "Mach-O container is not recognized as an arm64 executable"
"${TEST_BIN_DIR}/strings" "$WORK_DIR/minimal_macho-pack" > "$WORK_DIR/strings_macho_container.out"
assert_file_contains "$WORK_DIR/strings_macho_container.out" 'EXPACKM1' "Mach-O container does not include expack metadata"
if [ "$(uname -s 2>/dev/null || echo unknown)" = Darwin ] && command -v codesign >/dev/null 2>&1 && command -v otool >/dev/null 2>&1; then
    assert_command_succeeds codesign --verify --strict --verbose=4 "$WORK_DIR/minimal_macho-pack" > "$WORK_DIR/codesign_macho_container_verify.out" 2>&1
    codesign -dv "$WORK_DIR/minimal_macho-pack" > "$WORK_DIR/codesign_macho_container.out" 2>&1
    assert_file_contains "$WORK_DIR/codesign_macho_container.out" 'Signature=adhoc' "expack did not emit an ad-hoc signature for the Mach-O container"
    otool -l "$WORK_DIR/minimal_macho-pack" > "$WORK_DIR/otool_macho_container_signed.out"
    assert_file_contains "$WORK_DIR/otool_macho_container_signed.out" 'LC_CODE_SIGNATURE' "signed Mach-O container has no code-signature load command"
    if [ "$(uname -m 2>/dev/null || echo unknown)" = arm64 ] && [ -x "${TEST_BIN_DIR}/echo" ]; then
        assert_command_succeeds "${TEST_BIN_DIR}/expack" "${TEST_BIN_DIR}/echo" "$WORK_DIR/echo_macho.container" > "$WORK_DIR/expack_echo_macho_container.out"
        assert_file_contains "$WORK_DIR/expack_echo_macho_container.out" 'with codec lzrep' "arm64 Mach-O runner container did not use an LZREP payload mode"
        assert_command_succeeds codesign --verify --strict --verbose=4 "$WORK_DIR/echo_macho.container" > "$WORK_DIR/codesign_echo_macho_container_verify.out" 2>&1
        assert_command_succeeds "$WORK_DIR/echo_macho.container" expack-macho-ok > "$WORK_DIR/echo_macho_container_run.out"
        assert_file_contains "$WORK_DIR/echo_macho_container_run.out" '^expack-macho-ok$' "arm64 Mach-O runner container did not execute the original image"
        if [ -x "${TEST_BIN_DIR}/expack" ]; then
            assert_command_succeeds "${TEST_BIN_DIR}/expack" --all "${TEST_BIN_DIR}/expack" "$WORK_DIR/expack_macho_lz4.container" > "$WORK_DIR/expack_macho_lz4_container.out"
            assert_file_contains "$WORK_DIR/expack_macho_lz4_container.out" '^  lz4-block: payload ' "arm64 Mach-O --all mode did not evaluate the LZ4 payload mode"
            assert_file_contains "$WORK_DIR/expack_macho_lz4_container.out" 'wrote Mach-O prototype container' "arm64 Mach-O --all mode did not write a runnable container"
            assert_command_succeeds codesign --verify --strict --verbose=4 "$WORK_DIR/expack_macho_lz4.container" > "$WORK_DIR/codesign_expack_macho_lz4_container_verify.out" 2>&1
            assert_command_succeeds "$WORK_DIR/expack_macho_lz4.container" --analyze "${TEST_BIN_DIR}/expack" > "$WORK_DIR/expack_macho_lz4_container_run.out"
            assert_file_contains "$WORK_DIR/expack_macho_lz4_container_run.out" '^selected: ' "arm64 Mach-O LZ4 runner container did not execute the original image"
        fi
    fi
fi
assert_command_succeeds "${TEST_BIN_DIR}/expack" --macho-container "$WORK_DIR/minimal_macho" "$WORK_DIR/minimal_macho.flag-container" > "$WORK_DIR/expack_macho_flag_container.out"
assert_file_contains "$WORK_DIR/expack_macho_flag_container.out" 'wrote Mach-O prototype container' "expack did not keep --macho-container compatibility"

for pe_fixture in "$ROOT_DIR"/tests/fixtures/pe/*.exe; do
    pe_name=$(basename "$pe_fixture")
    assert_command_succeeds "${TEST_BIN_DIR}/file" "$pe_fixture" > "$WORK_DIR/file_pe_fixture_$pe_name.out"
    assert_file_contains "$WORK_DIR/file_pe_fixture_$pe_name.out" 'PE/COFF executable PE32+ x86-64' "PE fixture is not recognized as a PE32+ executable"
    assert_command_succeeds "${TEST_BIN_DIR}/expack" --analyze "$pe_fixture" > "$WORK_DIR/expack_pe_$pe_name.out"
    assert_file_contains "$WORK_DIR/expack_pe_$pe_name.out" 'format PE/COFF PE32+ x86-64' "expack did not analyze PE/COFF input"
    assert_file_contains "$WORK_DIR/expack_pe_$pe_name.out" '^selected: ' "expack did not select a PE/COFF compression candidate"
    assert_command_succeeds "${TEST_BIN_DIR}/expack" "$pe_fixture" "$WORK_DIR/$pe_name.pack" > "$WORK_DIR/expack_pe_pack_$pe_name.out"
    assert_file_contains "$WORK_DIR/expack_pe_pack_$pe_name.out" '^expack: input .*format PE/COFF PE32' "expack did not report PE/COFF input statistics while packing"
    assert_file_contains "$WORK_DIR/expack_pe_pack_$pe_name.out" '^  lzrep: payload ' "expack did not report PE/COFF compression candidates while packing"
    assert_file_contains "$WORK_DIR/expack_pe_pack_$pe_name.out" 'wrote PE/COFF ' "expack did not report PE/COFF output"
    assert_file_contains "$WORK_DIR/expack_pe_pack_$pe_name.out" 'output .* bytes, payload ' "expack did not report PE/COFF output statistics while packing"
    assert_command_succeeds "${TEST_BIN_DIR}/file" "$WORK_DIR/$pe_name.pack" > "$WORK_DIR/file_pe_pack_$pe_name.out"
    assert_file_contains "$WORK_DIR/file_pe_pack_$pe_name.out" 'PE/COFF executable PE32+ x86-64' "PE/COFF container is not recognized as a PE32+ executable"
    assert_command_succeeds "${TEST_BIN_DIR}/strings" "$WORK_DIR/$pe_name.pack" > "$WORK_DIR/strings_pe_pack_$pe_name.out"
    if grep -q 'self-extracting container' "$WORK_DIR/expack_pe_pack_$pe_name.out"; then
        assert_file_contains "$WORK_DIR/strings_pe_pack_$pe_name.out" 'EXPACKP1' "PE/COFF container does not include expack metadata"
    fi
done

{
    printf '\177ELF\002\001\001'
    dd if=/dev/zero bs=1 count=9 2>/dev/null
    printf '\002\000'
    printf '\076\000'
    printf '\001\000\000\000'
    dd if=/dev/zero bs=1 count=8 2>/dev/null
    printf '\200\000\000\000\000\000\000\000'
    dd if=/dev/zero bs=1 count=12 2>/dev/null
    printf '\100\000'
    printf '\070\000'
    printf '\001\000'
    dd if=/dev/zero bs=1 count=70 2>/dev/null
    printf '\001\000\000\000'
    printf '\005\000\000\000'
    dd if=/dev/zero bs=1 count=8 2>/dev/null
    printf '\000\000\100\000\000\000\000\000'
    printf '\000\000\100\000\000\000\000\000'
    printf '\100\000\000\000\000\000\000\000'
    printf '\100\000\000\000\000\000\000\000'
    printf '\000\020\000\000\000\000\000\000'
} > "$WORK_DIR/expack_bad_phdr.elf"
if "${TEST_BIN_DIR}/expack" --analyze "$WORK_DIR/expack_bad_phdr.elf" > "$WORK_DIR/expack_bad_phdr.out" 2> "$WORK_DIR/expack_bad_phdr.err"; then
    fail "expack should reject ELF files whose program headers are outside the reconstructed image"
fi
assert_file_contains "$WORK_DIR/expack_bad_phdr.err" 'ELF preprocessing failed' "expack did not report preprocessing failure for an out-of-image program header table"

assert_command_succeeds "${TEST_BIN_DIR}/file" -i "$WORK_DIR/text.txt" > "$WORK_DIR/file_mime.out"
assert_file_contains "$WORK_DIR/file_mime.out" 'text/plain' "file -i did not report a text mime type"

assert_command_succeeds "${TEST_BIN_DIR}/file" -b "$WORK_DIR/text.txt" > "$WORK_DIR/file_brief.out"
assert_file_contains "$WORK_DIR/file_brief.out" '^ASCII text$' "file -b did not suppress the filename prefix"

assert_command_succeeds "${TEST_BIN_DIR}/file" --verbose "$WORK_DIR/text.txt" > "$WORK_DIR/file_verbose.out"
assert_file_contains "$WORK_DIR/file_verbose.out" '^  type: ASCII text$' "file --verbose did not include the detected type"
assert_file_contains "$WORK_DIR/file_verbose.out" '^  mime: text/plain' "file --verbose did not include the MIME type"
assert_file_contains "$WORK_DIR/file_verbose.out" '^  size: 11 bytes$' "file --verbose did not include the file size"
assert_file_contains "$WORK_DIR/file_verbose.out" '^  mode: ' "file --verbose did not include the mode"

printf '\211PNG\r\n\032\n\000\000\000\rIHDR\000\000\000\002\000\000\000\003' > "$WORK_DIR/sample.png"
assert_command_succeeds "${TEST_BIN_DIR}/file" "$WORK_DIR/sample.png" > "$WORK_DIR/file_png.out"
assert_file_contains "$WORK_DIR/file_png.out" 'PNG image data, 2 x 3' "file did not report PNG dimensions"

{
    printf 'MZ'
    dd if=/dev/zero bs=1 count=58 2>/dev/null
    printf '\200\000\000\000'
    dd if=/dev/zero bs=1 count=64 2>/dev/null
    printf 'PE\000\000'
    printf '\144\206'
    printf '\003\000'
    dd if=/dev/zero bs=1 count=4 2>/dev/null
    printf '\120\002\000\000'
    printf '\001\000\000\000'
    printf '\360\000'
    printf '\042\000'
    printf '\013\002'
    printf '\016\000'
    printf '\000\002\000\000'
    dd if=/dev/zero bs=1 count=8 2>/dev/null
    printf '\000\020\000\000'
    printf '\000\020\000\000'
    printf '\000\000\000\100\001\000\000\000'
    printf '\000\020\000\000'
    printf '\000\002\000\000'
    printf '\006\000\000\000'
    dd if=/dev/zero bs=1 count=4 2>/dev/null
    printf '\006\000\000\000'
    dd if=/dev/zero bs=1 count=4 2>/dev/null
    printf '\000\100\000\000'
    printf '\000\002\000\000'
    dd if=/dev/zero bs=1 count=4 2>/dev/null
    printf '\003\000'
    printf '\140\201'
    dd if=/dev/zero bs=1 count=88 2>/dev/null
    printf '\000\060\000\000'
    printf '\034\000\000\000'
    dd if=/dev/zero bs=1 count=72 2>/dev/null
    printf '.text\000\000\000'
    printf '\043\001\000\000'
    printf '\000\020\000\000'
    printf '\020\000\000\000'
    printf '\000\002\000\000'
    dd if=/dev/zero bs=1 count=12 2>/dev/null
    printf '\040\000\000\140'
    printf '.rdata\000\000'
    printf '\200\000\000\000'
    printf '\000\040\000\000'
    printf '\020\000\000\000'
    printf '\020\002\000\000'
    dd if=/dev/zero bs=1 count=12 2>/dev/null
    printf '\100\000\000\100'
    printf '.pdata\000\000'
    printf '\100\000\000\000'
    printf '\000\060\000\000'
    printf '\020\000\000\000'
    printf '\040\002\000\000'
    dd if=/dev/zero bs=1 count=12 2>/dev/null
    printf '\100\000\000\100'
    printf '\220\220\303\000PE text data'
    printf 'PE rdata sample!'
    printf 'PE pdata sample!'
    printf 'PE_SYMBOL_TABLE!'
} > "$WORK_DIR/pe64.exe"
assert_command_succeeds "${TEST_BIN_DIR}/file" "$WORK_DIR/pe64.exe" > "$WORK_DIR/file_pe.out"
assert_file_contains "$WORK_DIR/file_pe.out" 'PE/COFF executable PE32+ x86-64, console, 3 sections' "file did not report PE/COFF executable details"
assert_command_succeeds "${TEST_BIN_DIR}/file" --verbose "$WORK_DIR/pe64.exe" > "$WORK_DIR/file_pe_verbose.out"
assert_file_contains "$WORK_DIR/file_pe_verbose.out" '^  magic: PE/COFF$' "file --verbose did not include the PE/COFF magic label"
assert_file_contains "$WORK_DIR/file_pe_verbose.out" '^  pe-entry-rva: 0x1000$' "file --verbose did not include the PE entry RVA"
assert_file_contains "$WORK_DIR/file_pe_verbose.out" '^  pe-image-base: 0x140000000$' "file --verbose did not include the PE image base"
assert_file_contains "$WORK_DIR/file_pe_verbose.out" 'dynamic-base' "file --verbose did not decode PE DLL characteristics"
assert_file_contains "$WORK_DIR/file_pe_verbose.out" '^    \.text: rva=0x1000' "file --verbose did not list PE sections"

assert_command_succeeds "${TEST_BIN_DIR}/objdump" -f -h "$WORK_DIR/pe64.exe" > "$WORK_DIR/objdump_pe.out"
assert_file_contains "$WORK_DIR/objdump_pe.out" 'file format pei-x86-64' "objdump -f did not identify PE/COFF files"
assert_file_contains "$WORK_DIR/objdump_pe.out" '^  0 \.text vaddr=0x1000' "objdump -h did not list PE sections"
assert_command_succeeds "${TEST_BIN_DIR}/objdump" -s "$WORK_DIR/pe64.exe" > "$WORK_DIR/objdump_pe_contents.out"
assert_file_contains "$WORK_DIR/objdump_pe_contents.out" '^Contents of section \.text$' "objdump -s did not dump PE section contents"
assert_file_contains "$WORK_DIR/objdump_pe_contents.out" '90 90 c3 00' "objdump -s did not include PE section bytes"
assert_command_succeeds "${TEST_BIN_DIR}/objdump" -t "$WORK_DIR/pe64.exe" > "$WORK_DIR/objdump_pe_symbols.out"
assert_file_contains "$WORK_DIR/objdump_pe_symbols.out" 'Symbol dumping for PE/COFF inputs is not implemented yet\.' "objdump -t did not report the PE symbol limitation"

assert_command_succeeds "${TEST_BIN_DIR}/strip" -v -o "$WORK_DIR/pe64.stripped.exe" "$WORK_DIR/pe64.exe" > "$WORK_DIR/strip_pe_verbose.out"
assert_file_contains "$WORK_DIR/strip_pe_verbose.out" 'PE/COFF' "strip -v did not report PE/COFF handling"
assert_file_contains "$WORK_DIR/strip_pe_verbose.out" 'cleared PE/COFF symbol table and debug directory' "strip did not report PE symbol/debug stripping"
assert_text_equals "$(${TEST_BIN_DIR}/od -An -t x1 -j 140 -N 8 "$WORK_DIR/pe64.stripped.exe" | "${TEST_BIN_DIR}/tr" -d ' \n')" "0000000000000000" "strip did not clear PE/COFF symbol table fields"
assert_text_equals "$(${TEST_BIN_DIR}/od -An -t x1 -j 312 -N 8 "$WORK_DIR/pe64.stripped.exe" | "${TEST_BIN_DIR}/tr" -d ' \n')" "0000000000000000" "strip did not clear PE/COFF debug data-directory fields"

assert_command_succeeds "${TEST_BIN_DIR}/strip" --dry-run -v -o "$WORK_DIR/pe64.dry.exe" "$WORK_DIR/pe64.exe" > "$WORK_DIR/strip_dry_run.out"
assert_file_contains "$WORK_DIR/strip_dry_run.out" 'dry-run' "strip --dry-run did not report dry-run mode"
if [ -e "$WORK_DIR/pe64.dry.exe" ]; then
    fail "strip --dry-run should not create an output file"
fi

assert_command_succeeds "${TEST_BIN_DIR}/od" "$WORK_DIR/text.txt" > "$WORK_DIR/od.out"
assert_file_contains "$WORK_DIR/od.out" '^0000000 ' "od did not print the expected offset prefix"

assert_command_succeeds "${TEST_BIN_DIR}/od" -A x -t x1 -j 1 -N 4 "$WORK_DIR/text.txt" > "$WORK_DIR/od_hex.out"
assert_file_contains "$WORK_DIR/od_hex.out" '^0000001 6c 70 68 61$' "od -A/-t/-j/-N did not dump the selected bytes as hex"

assert_command_succeeds "${TEST_BIN_DIR}/od" -A n -t c -N 2 "$WORK_DIR/text.txt" > "$WORK_DIR/od_char.out"
assert_file_contains "$WORK_DIR/od_char.out" '^  a   l$' "od -A n -t c did not render printable characters"

assert_command_succeeds "${TEST_BIN_DIR}/hexdump" "$WORK_DIR/text.txt" > "$WORK_DIR/hexdump.out"
assert_file_contains "$WORK_DIR/hexdump.out" '61 6c 70 68 61' "hexdump did not include the expected byte sequence"

assert_command_succeeds "${TEST_BIN_DIR}/hexdump" -C -s 6 -n 4 "$WORK_DIR/text.txt" > "$WORK_DIR/hexdump_slice.out"
assert_file_contains "$WORK_DIR/hexdump_slice.out" '^00000006  62 65 74 61' "hexdump -C/-s/-n did not dump the selected slice"
assert_file_contains "$WORK_DIR/hexdump_slice.out" '^0000000a$' "hexdump -n did not report the expected final offset"

printf '\001\002\003\004\377' > "$WORK_DIR/bytes.bin"
assert_command_succeeds "${TEST_BIN_DIR}/hexdump" -x "$WORK_DIR/bytes.bin" > "$WORK_DIR/hexdump_x.out"
assert_file_contains "$WORK_DIR/hexdump_x.out" '^00000000 0201 0403 00ff$' "hexdump -x did not render little-endian 16-bit hex words"

assert_command_succeeds "${TEST_BIN_DIR}/hexdump" -d -A d -s 16 -n 4 "$WORK_DIR/text.txt" > "$WORK_DIR/hexdump_decimal_address.out"
assert_file_contains "$WORK_DIR/hexdump_decimal_address.out" '^00000016$' "hexdump -A d did not print decimal final offsets after skipping past EOF"

assert_command_succeeds "${TEST_BIN_DIR}/hexdump" -o -A n "$WORK_DIR/bytes.bin" > "$WORK_DIR/hexdump_o_no_address.out"
assert_file_contains "$WORK_DIR/hexdump_o_no_address.out" '^001001 002003 000377$' "hexdump -o/-A n did not render octal words without addresses"

cat > "$WORK_DIR/probe.c" <<'EOF'
int main(void) {
    return 0;
}
EOF

assert_command_succeeds cc -o "$WORK_DIR/probe" "$WORK_DIR/probe.c"
assert_command_succeeds cc -c -o "$WORK_DIR/probe.o" "$WORK_DIR/probe.c"

assert_command_succeeds "${TEST_BIN_DIR}/file" "$WORK_DIR/probe" > "$WORK_DIR/file_probe.out"
if ! grep -q 'ELF ' "$WORK_DIR/file_probe.out"; then
    assert_file_contains "$WORK_DIR/file_probe.out" 'Mach-O ' "file did not report richer executable details"
fi

assert_command_succeeds "${TEST_BIN_DIR}/readelf" -h "$WORK_DIR/probe" > "$WORK_DIR/readelf.out"
if ! grep -q '^ELF Header:' "$WORK_DIR/readelf.out"; then
    assert_file_contains "$WORK_DIR/readelf.out" '^Mach-O Header:' "readelf -h did not print a recognizable object header"
fi
assert_file_contains "$WORK_DIR/readelf.out" 'Machine:' "readelf -h did not include the machine field"

assert_command_succeeds "${TEST_BIN_DIR}/objdump" -f "$WORK_DIR/probe" > "$WORK_DIR/objdump.out"
if ! grep -q 'file format elf' "$WORK_DIR/objdump.out"; then
    assert_file_contains "$WORK_DIR/objdump.out" 'file format mach-o' "objdump -f did not identify the hosted object format"
    assert_command_succeeds "${TEST_BIN_DIR}/objdump" -h "$WORK_DIR/probe" > "$WORK_DIR/objdump_macho_sections.out"
    assert_file_contains "$WORK_DIR/objdump_macho_sections.out" '^Sections:$' "objdump -h did not print Mach-O sections"
    assert_file_contains "$WORK_DIR/objdump_macho_sections.out" '__TEXT,__text' "objdump -h did not list the Mach-O text section"
    assert_command_succeeds "${TEST_BIN_DIR}/objdump" -s "$WORK_DIR/probe" > "$WORK_DIR/objdump_macho_contents.out"
    assert_file_contains "$WORK_DIR/objdump_macho_contents.out" '^Contents of section __TEXT,__text$' "objdump -s did not dump Mach-O section contents"
fi

assert_command_succeeds "${TEST_BIN_DIR}/strip" -v -o "$WORK_DIR/probe.stripped" "$WORK_DIR/probe" > "$WORK_DIR/strip_probe_verbose.out"
[ -f "$WORK_DIR/probe.stripped" ] || fail "strip did not create the requested output file"
orig_size=$(wc -c < "$WORK_DIR/probe" | tr -d ' ')
stripped_size=$(wc -c < "$WORK_DIR/probe.stripped" | tr -d ' ')
[ "$stripped_size" -le "$orig_size" ] || fail "strip output was unexpectedly larger than the original binary"
assert_file_contains "$WORK_DIR/strip_probe_verbose.out" 'action: ' "strip -v did not report the action taken"
if grep -q 'ELF ' "$WORK_DIR/file_probe.out"; then
    assert_file_contains "$WORK_DIR/strip_probe_verbose.out" 'ELF64 little-endian' "strip -v did not report ELF handling"
    assert_command_succeeds "${TEST_BIN_DIR}/readelf" -h "$WORK_DIR/probe.stripped" > "$WORK_DIR/readelf_stripped.out"
    assert_file_contains "$WORK_DIR/readelf_stripped.out" 'Section header offset:[[:space:]]*0' "strip did not clear the ELF section header offset"
else
    assert_file_contains "$WORK_DIR/strip_probe_verbose.out" 'Mach-O 64-bit little-endian' "strip -v did not report Mach-O handling"
    assert_file_contains "$WORK_DIR/strip_probe_verbose.out" 'mach-o-load-commands:' "strip -v did not include Mach-O load-command diagnostics"
    assert_file_contains "$WORK_DIR/strip_probe_verbose.out" 'mach-o-symtab:' "strip -v did not include Mach-O symbol diagnostics"
fi

if "${TEST_BIN_DIR}/strip" -o "$WORK_DIR/probe_obj.stripped" "$WORK_DIR/probe.o" >/dev/null 2>&1; then
    fail "strip should reject relocatable object files until section rewriting is implemented"
fi

assert_command_succeeds "${TEST_BIN_DIR}/ar" r "$WORK_DIR/probe.a" "$WORK_DIR/probe.o" >/dev/null
assert_command_succeeds "${TEST_BIN_DIR}/strip" --dry-run -v "$WORK_DIR/probe.a" > "$WORK_DIR/strip_archive_verbose.out"
assert_file_contains "$WORK_DIR/strip_archive_verbose.out" 'ar archive' "strip -v did not identify ar archives"
assert_file_contains "$WORK_DIR/strip_archive_verbose.out" 'archive-members:' "strip -v did not report archive member diagnostics"
