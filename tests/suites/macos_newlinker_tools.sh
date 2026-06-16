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
    checked_tools=0
    for candidate in "$BUILD_DIR"/*; do
        if [ -f "$candidate" ] && [ -x "$candidate" ]; then
            checked_tools=$((checked_tools + 1))
            otool -L "$candidate" > "$WORK_DIR/otool_L.out"
            if grep -q '\.dylib' "$WORK_DIR/otool_L.out"; then
                fail "project-linked Mach-O tool should not import dylibs: $candidate"
            fi
        fi
    done
    [ "$checked_tools" -gt 0 ] || fail "no project-linked Mach-O tools were checked"

    otool -l "$BUILD_DIR/true" > "$WORK_DIR/true_load_commands.otool"
    assert_file_contains "$WORK_DIR/true_load_commands.otool" 'LC_BUILD_VERSION' "project-linked Mach-O output should keep LC_BUILD_VERSION"
    assert_file_contains "$WORK_DIR/true_load_commands.otool" 'ntools 0' "default macOS newlinker flags should use compact build-version load command"
fi

"$BUILD_DIR/file" "$BUILD_DIR/true" > "$WORK_DIR/file_true.out"
assert_file_contains "$WORK_DIR/file_true.out" 'Mach-O 64-bit executable arm64' "file did not identify a newlinker Mach-O executable"

"$BUILD_DIR/file" --json "$BUILD_DIR/true" > "$WORK_DIR/file_true.jsonl"
assert_file_contains "$WORK_DIR/file_true.jsonl" '"event":"file_type"' "file --json did not emit a file_type event"
assert_file_contains "$WORK_DIR/file_true.jsonl" '"magic":"Mach-O"' "file --json did not report Mach-O magic"

"$BUILD_DIR/readelf" -h -l -S -r "$BUILD_DIR/true" > "$WORK_DIR/readelf_true.out"
assert_file_contains "$WORK_DIR/readelf_true.out" 'Mach-O Header' "readelf did not print the Mach-O header"
assert_file_contains "$WORK_DIR/readelf_true.out" 'LC_DYLD_INFO_ONLY' "readelf did not report Mach-O dyld rebase metadata"
assert_file_contains "$WORK_DIR/readelf_true.out" 'rebase_size=0x0' "readelf did not report empty Mach-O rebase metadata for true"
assert_file_contains "$WORK_DIR/readelf_true.out" 'There are no relocations in this Mach-O file' "readelf did not handle Mach-O relocation output"

"$BUILD_DIR/readelf" -n "$BUILD_DIR/true" > "$WORK_DIR/readelf_true_signature.out"
assert_file_contains "$WORK_DIR/readelf_true_signature.out" 'CodeDirectory SHA-256 hashes verified' "readelf did not verify Mach-O CodeDirectory hashes"
"$BUILD_DIR/readelf" --compare "$BUILD_DIR/true" "$BUILD_DIR/true" > "$WORK_DIR/readelf_compare_equal.out"
if "$BUILD_DIR/readelf" --compare "$BUILD_DIR/true" "$BUILD_DIR/false" > "$WORK_DIR/readelf_compare_different.out" 2>&1; then
    fail "readelf --compare should fail for different Mach-O tools"
fi
assert_file_contains "$WORK_DIR/readelf_compare_different.out" 'sha256' "readelf --compare did not report a content difference"
"$BUILD_DIR/readelf" --json -h -l -S -n "$BUILD_DIR/true" > "$WORK_DIR/readelf_true.jsonl"
assert_file_contains "$WORK_DIR/readelf_true.jsonl" '"name":"LC_DYLD_INFO_ONLY"' "readelf --json did not emit Mach-O dyld-info load command"
assert_file_contains "$WORK_DIR/readelf_true.jsonl" '"rebase_size":0' "readelf --json did not emit Mach-O rebase size"
assert_file_contains "$WORK_DIR/readelf_true.jsonl" '"event":"macho_code_signature"' "readelf --json did not emit Mach-O code-signature information"

"$BUILD_DIR/readelf" --macho-map "$BUILD_DIR/true" > "$WORK_DIR/readelf_true_map.out"
assert_file_contains "$WORK_DIR/readelf_true_map.out" 'Mach-O File/VM Map' "readelf --macho-map did not print the Mach-O map"
assert_file_contains "$WORK_DIR/readelf_true_map.out" 'Segment __TEXT' "readelf --macho-map did not report the __TEXT segment"

"$BUILD_DIR/readelf" --signature-details "$BUILD_DIR/true" > "$WORK_DIR/readelf_true_signature_details.out"
assert_file_contains "$WORK_DIR/readelf_true_signature_details.out" 'Mach-O Signature Details' "readelf --signature-details did not print detailed signature information"
assert_file_contains "$WORK_DIR/readelf_true_signature_details.out" 'CodeDirectory hash offset' "readelf --signature-details did not report CodeDirectory offsets"

true_text_addr=$(sed -n 's/.*__TEXT,__text.* addr=\(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' "$WORK_DIR/readelf_true.out" | sed -n '1p')
[ -n "$true_text_addr" ] || fail "readelf did not report a __TEXT,__text address for true"
"$BUILD_DIR/readelf" --explain-address "$true_text_addr" "$BUILD_DIR/true" > "$WORK_DIR/readelf_true_explain.out"
assert_file_contains "$WORK_DIR/readelf_true_explain.out" 'Mach-O Address Explanation' "readelf --explain-address did not explain a Mach-O address"
assert_file_contains "$WORK_DIR/readelf_true_explain.out" 'section: __TEXT,__text' "readelf --explain-address did not locate the __text section"

"$BUILD_DIR/readelf" --compare --deep "$BUILD_DIR/true" "$BUILD_DIR/true" > "$WORK_DIR/readelf_compare_deep_equal.out"
if "$BUILD_DIR/readelf" --compare --deep "$BUILD_DIR/true" "$BUILD_DIR/false" > "$WORK_DIR/readelf_compare_deep_different.out" 2>&1; then
    fail "readelf --compare --deep should fail for different Mach-O tools"
fi
assert_file_contains "$WORK_DIR/readelf_compare_deep_different.out" 'load_commands_sha256' "readelf --compare --deep did not report load-command differences"

"$BUILD_DIR/strace" -e open,read,write "$BUILD_DIR/cat" "$WORK_DIR/input.txt" > "$WORK_DIR/strace_cat.stdout" 2> "$WORK_DIR/strace_cat.stderr"
assert_files_equal "$WORK_DIR/input.txt" "$WORK_DIR/strace_cat.stdout" "strace should preserve traced command stdout"
assert_file_contains "$WORK_DIR/strace_cat.stderr" 'open("' "macOS strace did not decode open path arguments"
assert_file_contains "$WORK_DIR/strace_cat.stderr" 'O_RDONLY' "macOS strace did not decode open flags"
assert_file_contains "$WORK_DIR/strace_cat.stderr" 'read(0x' "macOS strace did not trace read calls"
assert_file_contains "$WORK_DIR/strace_cat.stderr" 'write(0x' "macOS strace did not trace write calls"
"$BUILD_DIR/strace" -p -T -e write "$BUILD_DIR/echo" hello > "$WORK_DIR/strace_echo_pid.stdout" 2> "$WORK_DIR/strace_echo_pid.stderr"
assert_file_contains "$WORK_DIR/strace_echo_pid.stdout" '^hello$' "strace should preserve echo stdout"
assert_file_contains "$WORK_DIR/strace_echo_pid.stderr" '^\[[0-9][0-9]*\] write' "macOS strace -p did not prefix pid"
assert_file_contains "$WORK_DIR/strace_echo_pid.stderr" '<0\.000 ms>' "macOS strace -T did not print the stable write duration placeholder"
"$BUILD_DIR/strace" -c "$BUILD_DIR/echo" hello > "$WORK_DIR/strace_echo_summary.stdout" 2> "$WORK_DIR/strace_echo_summary.stderr"
assert_file_contains "$WORK_DIR/strace_echo_summary.stderr" '^syscall calls errors bytes total_ms$' "macOS strace -c did not print summary header"
assert_file_contains "$WORK_DIR/strace_echo_summary.stderr" '^write 1 0 ' "macOS strace -c did not summarize write calls"
"$BUILD_DIR/strace" -o "$WORK_DIR/strace_echo_output.trace" -e write "$BUILD_DIR/echo" hello > "$WORK_DIR/strace_echo_output.stdout" 2> "$WORK_DIR/strace_echo_output.stderr"
assert_file_contains "$WORK_DIR/strace_echo_output.stdout" '^hello$' "strace -o should preserve target stdout"
assert_file_contains "$WORK_DIR/strace_echo_output.trace" '^write' "strace -o did not write trace output file"
if [ -s "$WORK_DIR/strace_echo_output.stderr" ]; then
    fail "strace -o should not write trace lines to stderr"
fi
"$BUILD_DIR/strace" --json -p -T -e write "$BUILD_DIR/echo" hello > "$WORK_DIR/strace_echo_json.stdout" 2> "$WORK_DIR/strace_echo_json.stderr"
assert_file_contains "$WORK_DIR/strace_echo_json.stderr" '"event":"syscall"' "macOS strace --json did not emit syscall events"
assert_file_contains "$WORK_DIR/strace_echo_json.stderr" '"pid":' "macOS strace --json did not include pid metadata"
assert_file_contains "$WORK_DIR/strace_echo_json.stderr" '"args":' "macOS strace --json did not include syscall arguments"

"$BUILD_DIR/size" -m "$BUILD_DIR/true" > "$WORK_DIR/size_true_segments.out"
assert_file_contains "$WORK_DIR/size_true_segments.out" 'Segment __TEXT' "size -m did not print Mach-O segment sizes"
assert_file_contains "$WORK_DIR/size_true_segments.out" 'Section __text' "size -m did not print Mach-O section sizes"
"$BUILD_DIR/size" --json -m "$BUILD_DIR/true" > "$WORK_DIR/size_true_segments.jsonl"
assert_file_contains "$WORK_DIR/size_true_segments.jsonl" '"event":"macho_segment_size"' "size --json -m did not emit Mach-O segment-size events"

TOOLS="true" NEWOS_MACOS_NEWLINKER_BUILD_DIR="$BUILD_DIR" bash "$ROOT_DIR/scripts/report-macos-freestanding-size.sh" > "$WORK_DIR/macos_size_report.tsv"
assert_file_contains "$WORK_DIR/macos_size_report.tsv" 'top_file_sections' "macOS size report did not include top-section diagnostics"
assert_file_contains "$WORK_DIR/macos_size_report.tsv" '__TEXT,__text=' "macOS size report did not report the top Mach-O text section"
assert_file_contains "$WORK_DIR/macos_size_report.tsv" 'unavailable' "macOS size report should mark map-derived attribution unavailable without maps"

"$BUILD_DIR/objdump" -f -h -r "$BUILD_DIR/true" > "$WORK_DIR/objdump_true.out"
assert_file_contains "$WORK_DIR/objdump_true.out" 'file format mach-o-64' "objdump did not print the Mach-O file format"
assert_file_contains "$WORK_DIR/objdump_true.out" 'No Mach-O relocations are available' "objdump did not handle Mach-O relocation output"
"$BUILD_DIR/objdump" --json -f -h -r "$BUILD_DIR/true" > "$WORK_DIR/objdump_true.jsonl"
assert_file_contains "$WORK_DIR/objdump_true.jsonl" '"event":"file_header"' "objdump --json did not emit a file_header event"
assert_file_contains "$WORK_DIR/objdump_true.jsonl" '"event":"section"' "objdump --json did not emit section events"

"$BUILD_DIR/imgcheck" "$BUILD_DIR/true" > "$WORK_DIR/imgcheck_true.out"
assert_file_contains "$WORK_DIR/imgcheck_true.out" 'OK (macho)' "imgcheck did not accept the project-linked Mach-O executable"
"$BUILD_DIR/imgcheck" --json "$BUILD_DIR/true" > "$WORK_DIR/imgcheck_true.jsonl"
assert_file_contains "$WORK_DIR/imgcheck_true.jsonl" '"code_signature_verified":true' "imgcheck --json did not report verified Mach-O code signature"

if [ -e /usr/bin/true ]; then
    "$BUILD_DIR/file" --json /usr/bin/true > "$WORK_DIR/file_usr_bin_true.jsonl"
    assert_file_contains "$WORK_DIR/file_usr_bin_true.jsonl" 'Mach-O universal binary with' "file --json did not describe /usr/bin/true as a universal Mach-O"
    assert_file_contains "$WORK_DIR/file_usr_bin_true.jsonl" 'arm64e' "file --json did not report the arm64e slice in /usr/bin/true"

    "$BUILD_DIR/readelf" -h -l -S -s -n /usr/bin/true > "$WORK_DIR/readelf_usr_bin_true.out"
    assert_file_contains "$WORK_DIR/readelf_usr_bin_true.out" 'Mach-O Universal Binary' "readelf did not report the Mach-O fat header for /usr/bin/true"
    assert_file_contains "$WORK_DIR/readelf_usr_bin_true.out" 'Selected Mach-O slice: arm64e' "readelf did not select the arm64e slice in /usr/bin/true"
    assert_file_contains "$WORK_DIR/readelf_usr_bin_true.out" 'LC_DYLD_CHAINED_FIXUPS' "readelf did not name dyld chained fixups"
    assert_file_contains "$WORK_DIR/readelf_usr_bin_true.out" 'LC_DYLD_EXPORTS_TRIE' "readelf did not name dyld exports trie"
    assert_file_contains "$WORK_DIR/readelf_usr_bin_true.out" 'LC_LOAD_DYLIB' "readelf did not decode dylib imports"
    assert_file_contains "$WORK_DIR/readelf_usr_bin_true.out" 'CDHash:' "readelf did not print the Mach-O CDHash"
    assert_file_contains "$WORK_DIR/readelf_usr_bin_true.out" 'CMS Signature' "readelf did not report the CMS signature blob"

    "$BUILD_DIR/readelf" --json -h -l -S -s -n /usr/bin/true > "$WORK_DIR/readelf_usr_bin_true.jsonl"
    assert_file_contains "$WORK_DIR/readelf_usr_bin_true.jsonl" '"event":"macho_fat_arch"' "readelf --json did not emit Mach-O fat architecture events"
    assert_file_contains "$WORK_DIR/readelf_usr_bin_true.jsonl" '"name":"LC_DYLD_CHAINED_FIXUPS"' "readelf --json did not name dyld chained fixups"
    assert_file_contains "$WORK_DIR/readelf_usr_bin_true.jsonl" '"cms_signature_size"' "readelf --json did not report CMS signature size"

    "$BUILD_DIR/readelf" --macho-fixups /usr/bin/true > "$WORK_DIR/readelf_usr_bin_true_fixups.out"
    assert_file_contains "$WORK_DIR/readelf_usr_bin_true_fixups.out" 'Mach-O Chained Fixups' "readelf --macho-fixups did not report chained fixups for /usr/bin/true"
    assert_file_contains "$WORK_DIR/readelf_usr_bin_true_fixups.out" 'fixup_pages=' "readelf --macho-fixups did not summarize fixup pages"

    "$BUILD_DIR/objdump" -f -h -t /usr/bin/true > "$WORK_DIR/objdump_usr_bin_true.out"
    assert_file_contains "$WORK_DIR/objdump_usr_bin_true.out" 'file format mach-o-64' "objdump did not inspect the selected /usr/bin/true Mach-O slice"
    assert_file_contains "$WORK_DIR/objdump_usr_bin_true.out" '__mh_execute_header' "objdump did not print /usr/bin/true Mach-O symbols"

    "$BUILD_DIR/nm" /usr/bin/true > "$WORK_DIR/nm_usr_bin_true.out"
    assert_file_contains "$WORK_DIR/nm_usr_bin_true.out" '__mh_execute_header' "nm did not inspect the selected /usr/bin/true Mach-O slice"

    "$BUILD_DIR/size" /usr/bin/true > "$WORK_DIR/size_usr_bin_true.out"
    assert_file_contains "$WORK_DIR/size_usr_bin_true.out" '/usr/bin/true' "size did not inspect the selected /usr/bin/true Mach-O slice"

    "$BUILD_DIR/imgcheck" --json /usr/bin/true > "$WORK_DIR/imgcheck_usr_bin_true.jsonl"
    assert_file_contains "$WORK_DIR/imgcheck_usr_bin_true.jsonl" '"code_signature_verified":true' "imgcheck did not verify the selected /usr/bin/true Mach-O slice"
fi

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