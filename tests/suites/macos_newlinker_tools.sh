#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

BUILD_DIR=${NEWOS_MACOS_NEWLINKER_BUILD_DIR:-$ROOT_DIR/build/newlinker-macos-aarch64}
WORK_DIR="$ROOT_DIR/tests/tmp/macos_newlinker_tools"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

note "macos newlinker tools"

assert_command_succeeds "$BUILD_DIR/true"
if "$BUILD_DIR/false"; then
    fail "false returned success"
fi

"$BUILD_DIR/echo" hello world > "$WORK_DIR/echo.out"
assert_file_contains "$WORK_DIR/echo.out" '^hello world$' "echo output mismatch"

"$BUILD_DIR/printf" 'hi %d' 7 > "$WORK_DIR/printf.out"
assert_file_contains "$WORK_DIR/printf.out" '^hi 7$' "printf output mismatch"

"$BUILD_DIR/rev" > "$WORK_DIR/rev.out" <<'EOF'
abc
EOF
assert_file_contains "$WORK_DIR/rev.out" '^cba$' "rev output mismatch"

"$BUILD_DIR/seq" 1 3 > "$WORK_DIR/seq.out"
assert_file_contains "$WORK_DIR/seq.out" '^1$' "seq output missing 1"
assert_file_contains "$WORK_DIR/seq.out" '^3$' "seq output missing 3"

printf 'one\ntwo\n' > "$WORK_DIR/input.txt"
"$BUILD_DIR/cat" "$WORK_DIR/input.txt" > "$WORK_DIR/cat.out"
assert_files_equal "$WORK_DIR/input.txt" "$WORK_DIR/cat.out" "cat output mismatch"

"$BUILD_DIR/basename" /tmp/file.txt > "$WORK_DIR/basename.out"
assert_file_contains "$WORK_DIR/basename.out" '^file.txt$' "basename output mismatch"

"$BUILD_DIR/dirname" /tmp/a/file.txt > "$WORK_DIR/dirname.out"
assert_file_contains "$WORK_DIR/dirname.out" '^/tmp/a$' "dirname output mismatch"

"$BUILD_DIR/cut" -c 2 "$WORK_DIR/input.txt" > "$WORK_DIR/cut.out"
assert_file_contains "$WORK_DIR/cut.out" '^n$' "cut output missing n"
assert_file_contains "$WORK_DIR/cut.out" '^w$' "cut output missing w"

printf 'abc' | "$BUILD_DIR/tr" a-z A-Z > "$WORK_DIR/tr.out"
assert_file_contains "$WORK_DIR/tr.out" '^ABC$' "tr output mismatch"

"$BUILD_DIR/wc" -l "$WORK_DIR/input.txt" > "$WORK_DIR/wc.out"
assert_file_contains "$WORK_DIR/wc.out" ' 2 ' "wc line count mismatch"

if command -v otool >/dev/null 2>&1; then
    otool -L "$BUILD_DIR/rev" > "$WORK_DIR/rev.otool"
    if grep -q '\.dylib' "$WORK_DIR/rev.otool"; then
        fail "project-linked Mach-O tool should not import dylibs yet"
    fi
fi

"$BUILD_DIR/file" "$BUILD_DIR/true" > "$WORK_DIR/file_true.out"
assert_file_contains "$WORK_DIR/file_true.out" 'Mach-O 64-bit executable arm64' "file did not identify a newlinker Mach-O executable"

"$BUILD_DIR/file" --json "$BUILD_DIR/true" > "$WORK_DIR/file_true.jsonl"
assert_file_contains "$WORK_DIR/file_true.jsonl" '"event":"file_type"' "file --json did not emit a file_type event"
assert_file_contains "$WORK_DIR/file_true.jsonl" '"magic":"Mach-O"' "file --json did not report Mach-O magic"

"$BUILD_DIR/readelf" -h -l -S -r "$BUILD_DIR/true" > "$WORK_DIR/readelf_true.out"
assert_file_contains "$WORK_DIR/readelf_true.out" 'Mach-O Header' "readelf did not print the Mach-O header"
assert_file_contains "$WORK_DIR/readelf_true.out" 'There are no relocations in this Mach-O file' "readelf did not handle Mach-O relocation output"

"$BUILD_DIR/readelf" -n "$BUILD_DIR/true" > "$WORK_DIR/readelf_true_signature.out"
assert_file_contains "$WORK_DIR/readelf_true_signature.out" 'CodeDirectory SHA-256 hashes verified' "readelf did not verify Mach-O CodeDirectory hashes"
"$BUILD_DIR/readelf" --compare "$BUILD_DIR/true" "$BUILD_DIR/true" > "$WORK_DIR/readelf_compare_equal.out"
if "$BUILD_DIR/readelf" --compare "$BUILD_DIR/true" "$BUILD_DIR/false" > "$WORK_DIR/readelf_compare_different.out" 2>&1; then
    fail "readelf --compare should fail for different Mach-O tools"
fi
assert_file_contains "$WORK_DIR/readelf_compare_different.out" 'sha256' "readelf --compare did not report a content difference"
"$BUILD_DIR/readelf" --json -h -l -S -n "$BUILD_DIR/true" > "$WORK_DIR/readelf_true.jsonl"
assert_file_contains "$WORK_DIR/readelf_true.jsonl" '"event":"macho_code_signature"' "readelf --json did not emit Mach-O code-signature information"

"$BUILD_DIR/objdump" -f -h -r "$BUILD_DIR/true" > "$WORK_DIR/objdump_true.out"
assert_file_contains "$WORK_DIR/objdump_true.out" 'file format mach-o-64' "objdump did not print the Mach-O file format"
assert_file_contains "$WORK_DIR/objdump_true.out" 'No Mach-O relocations are available' "objdump did not handle Mach-O relocation output"
"$BUILD_DIR/objdump" --json -f -h -r "$BUILD_DIR/true" > "$WORK_DIR/objdump_true.jsonl"
assert_file_contains "$WORK_DIR/objdump_true.jsonl" '"event":"file_header"' "objdump --json did not emit a file_header event"
assert_file_contains "$WORK_DIR/objdump_true.jsonl" '"event":"section"' "objdump --json did not emit section events"

"$BUILD_DIR/imgcheck" "$BUILD_DIR/true" > "$WORK_DIR/imgcheck_true.out"
assert_file_contains "$WORK_DIR/imgcheck_true.out" 'PIE has no dyld rebase metadata' "imgcheck did not report the Mach-O PIE/rebase warning"
assert_file_contains "$WORK_DIR/imgcheck_true.out" 'code-signature=verified' "imgcheck did not verify the Mach-O code signature"
"$BUILD_DIR/imgcheck" --json "$BUILD_DIR/true" > "$WORK_DIR/imgcheck_true.jsonl"
assert_file_contains "$WORK_DIR/imgcheck_true.jsonl" '"code_signature_verified":true' "imgcheck --json did not report verified Mach-O code signature"

if command -v clang >/dev/null 2>&1; then
    cat > "$WORK_DIR/macho_symbols.c" <<'C'
extern int ext;
int *p = &ext;
int f(void) { return ext + 1; }
C
    if clang -target arm64-apple-macos11 -c "$WORK_DIR/macho_symbols.c" -o "$WORK_DIR/macho_symbols.o" > "$WORK_DIR/macho_symbols_compile.out" 2>&1; then
        "$BUILD_DIR/nm" "$WORK_DIR/macho_symbols.o" > "$WORK_DIR/nm_macho_symbols.out"
        assert_file_contains "$WORK_DIR/nm_macho_symbols.out" '_ext' "nm did not print Mach-O undefined symbols"
        assert_file_contains "$WORK_DIR/nm_macho_symbols.out" '_f' "nm did not print Mach-O defined symbols"
        "$BUILD_DIR/nm" --json "$WORK_DIR/macho_symbols.o" > "$WORK_DIR/nm_macho_symbols.jsonl"
        assert_file_contains "$WORK_DIR/nm_macho_symbols.jsonl" '"format":"macho"' "nm --json did not identify Mach-O format"
        assert_file_contains "$WORK_DIR/nm_macho_symbols.jsonl" '"name":"_ext"' "nm --json did not emit Mach-O symbols"

        "$BUILD_DIR/readelf" -r "$WORK_DIR/macho_symbols.o" > "$WORK_DIR/readelf_macho_symbols.out"
        assert_file_contains "$WORK_DIR/readelf_macho_symbols.out" 'GOT_LOAD_PAGE21' "readelf did not decode Mach-O arm64 relocations"

        "$BUILD_DIR/objdump" -r "$WORK_DIR/macho_symbols.o" > "$WORK_DIR/objdump_macho_symbols.out"
        assert_file_contains "$WORK_DIR/objdump_macho_symbols.out" 'RELOCATION RECORDS FOR' "objdump did not print Mach-O relocations"
        "$BUILD_DIR/objdump" --json -f -h -t -r "$WORK_DIR/macho_symbols.o" > "$WORK_DIR/objdump_macho_symbols.jsonl"
        assert_file_contains "$WORK_DIR/objdump_macho_symbols.jsonl" '"event":"relocation"' "objdump --json did not emit Mach-O relocations"
    fi
fi

echo "MACOS_NEWLINKER_TOOLS_OK"