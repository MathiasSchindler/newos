#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"
. "$ROOT_DIR/tests/lib/build.sh"

BIN_DIR=$(newos_test_tool_dir)
WORK_DIR="$ROOT_DIR/tests/tmp/freestanding"

find_loopback_port() {
    if command -v python3 >/dev/null 2>&1; then
        python3 - <<'PY'
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
finally:
    sock.close()
PY
        return
    fi
    if command -v python >/dev/null 2>&1; then
        python - <<'PY'
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
finally:
    sock.close()
PY
        return
    fi

    echo $((31000 + ($$ % 1000)))
}

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

note "freestanding binaries"

if [ ! -x "$BIN_DIR/sh" ] || [ ! -x "$BIN_DIR/httpd" ]; then
    fail "freestanding build directory is missing expected tools: $BIN_DIR"
fi

"$BIN_DIR/readelf" -h "$BIN_DIR/yes" > "$WORK_DIR/project_readelf_yes.out"
if grep -q '^ELF Header:$' "$WORK_DIR/project_readelf_yes.out"; then
    assert_file_contains "$WORK_DIR/project_readelf_yes.out" 'Program header offset:[[:space:]]*0x3a' "project readelf should report overlapped tiny ELF program headers"
    assert_file_contains "$WORK_DIR/project_readelf_yes.out" 'Section headers:[[:space:]]*0' "project readelf should report sectionless freestanding ELF files"
    assert_file_contains "$WORK_DIR/project_readelf_yes.out" 'Section header entry size:.*ignored; no section headers' "project readelf should treat section fields as ignored when sectionless"
    assert_file_contains "$WORK_DIR/project_readelf_yes.out" 'Section header string table index:[[:space:]]*ignored' "project readelf should ignore the section string table index when sectionless"

    "$BIN_DIR/file" "$BIN_DIR/yes" > "$WORK_DIR/project_file_yes.out"
    assert_file_contains "$WORK_DIR/project_file_yes.out" 'ELF 64-bit LSB executable, x86-64' "project file should accept overlapped sectionless freestanding ELF files"

    "$BIN_DIR/objdump" -f -h "$BIN_DIR/yes" > "$WORK_DIR/project_objdump_yes.out"
    assert_file_contains "$WORK_DIR/project_objdump_yes.out" 'file format elf64-x86-64' "project objdump should accept overlapped sectionless freestanding ELF files"
    assert_file_contains "$WORK_DIR/project_objdump_yes.out" '^Sections:$' "project objdump should print the section table heading for sectionless ELF files"

    "$BIN_DIR/readelf" -S "$BIN_DIR/yes" > "$WORK_DIR/project_readelf_yes_sections.out"
    assert_file_contains "$WORK_DIR/project_readelf_yes_sections.out" '^Section Headers:$' "project readelf -S should handle sectionless freestanding ELF files"
    assert_file_contains "$WORK_DIR/project_readelf_yes_sections.out" '(none)' "project readelf -S should report no sections for sectionless freestanding ELF files"

    "$BIN_DIR/readelf" -l "$BIN_DIR/yes" > "$WORK_DIR/project_readelf_yes_programs.out"
    assert_file_contains "$WORK_DIR/project_readelf_yes_programs.out" '^Program Headers:$' "project readelf -l should print program headers"
    assert_file_contains "$WORK_DIR/project_readelf_yes_programs.out" 'LOAD off=0x0 .*filesz=.*memsz=.*flags=R-E' "project readelf -l should describe the newlinker load segment"
    assert_file_contains "$WORK_DIR/project_readelf_yes_programs.out" 'align=0x1' "project readelf -l should report tiny ELF load alignment"

    "$BIN_DIR/readelf" -a "$BIN_DIR/yes" > "$WORK_DIR/project_readelf_yes_all.out"
    assert_file_contains "$WORK_DIR/project_readelf_yes_all.out" 'There is no dynamic section in this file' "project readelf -a should report missing dynamic section"
    assert_file_contains "$WORK_DIR/project_readelf_yes_all.out" 'There are no relocations in this file' "project readelf -a should report missing relocations"
    assert_file_contains "$WORK_DIR/project_readelf_yes_all.out" 'No symbol table is available' "project readelf -a should report missing symbols"
    assert_file_contains "$WORK_DIR/project_readelf_yes_all.out" 'No notes found in this file' "project readelf -a should report missing notes"
else
    assert_file_contains "$WORK_DIR/project_readelf_yes.out" '^Mach-O Header:$' "project readelf should parse freestanding Mach-O outputs on macOS"
    assert_file_contains "$WORK_DIR/project_readelf_yes.out" 'Machine:[[:space:]]*AArch64' "project readelf should report AArch64 for freestanding macOS binaries"
fi

if command -v readelf >/dev/null 2>&1; then
    LC_ALL=C readelf -h "$BIN_DIR/httpd" > "$WORK_DIR/httpd.elf"
    assert_file_contains "$WORK_DIR/httpd.elf" 'Type:[[:space:]]*\(EXEC\|DYN\)' "freestanding httpd should be a static executable"
    LC_ALL=C readelf -lW "$BIN_DIR/httpd" > "$WORK_DIR/httpd.phdr"
    if grep -q 'INTERP' "$WORK_DIR/httpd.phdr"; then
        fail "freestanding httpd should not request a dynamic interpreter"
    fi
    if grep -q 'GNU_STACK' "$WORK_DIR/httpd.phdr"; then
        assert_file_contains "$WORK_DIR/httpd.phdr" 'GNU_STACK[[:space:]].* RW ' "freestanding httpd stack should be non-executable"
    fi
fi

"$BIN_DIR/echo" "hello freestanding" > "$WORK_DIR/echo.out"
assert_file_contains "$WORK_DIR/echo.out" '^hello freestanding$' "freestanding echo did not run"

printf 'gamma\nalpha\nbeta\n' > "$WORK_DIR/sort.in"
"$BIN_DIR/sort" "$WORK_DIR/sort.in" > "$WORK_DIR/sort.out"
cat > "$WORK_DIR/sort.expected" <<'EOF'
alpha
beta
gamma
EOF
assert_files_equal "$WORK_DIR/sort.expected" "$WORK_DIR/sort.out" "freestanding sort output mismatch"

printf 'alpha\nbeta\n' | "$BIN_DIR/grep" beta > "$WORK_DIR/grep.out"
assert_file_contains "$WORK_DIR/grep.out" '^beta$' "freestanding grep did not match stdin"

PATH="$BIN_DIR" "$BIN_DIR/which" ls sql > "$WORK_DIR/which.out"
cat > "$WORK_DIR/which.expected" <<EOF
$BIN_DIR/ls
$BIN_DIR/sql
EOF
assert_files_equal "$WORK_DIR/which.expected" "$WORK_DIR/which.out" "freestanding which did not honor PATH"

cat > "$WORK_DIR/script.sh" <<EOF
$BIN_DIR/echo shell-ok
$BIN_DIR/printf '%s\\n' pipe-ok | $BIN_DIR/tr a-z A-Z
EOF
"$BIN_DIR/sh" "$WORK_DIR/script.sh" > "$WORK_DIR/sh.out"
assert_file_contains "$WORK_DIR/sh.out" '^shell-ok$' "freestanding shell did not run an absolute command"
assert_file_contains "$WORK_DIR/sh.out" '^PIPE-OK$' "freestanding shell pipeline failed"

mkdir -p "$WORK_DIR/http_root"
printf 'hello from freestanding httpd\n' > "$WORK_DIR/http_root/index.txt"
HTTP_PORT=$(find_loopback_port 2>"$WORK_DIR/tcp_probe.err" || true)
if [ -z "$HTTP_PORT" ]; then
    note "freestanding httpd/wget loopback skipped: local TCP listeners unavailable"
else
    "$BIN_DIR/httpd" -p "$HTTP_PORT" -r "$WORK_DIR/http_root" -i index.txt > "$WORK_DIR/httpd.log" 2>&1 &
    httpd_pid=$!
    trap 'kill "$httpd_pid" 2>/dev/null || true' EXIT INT TERM

    fetch_status=1
    attempt=0
    while [ "$attempt" -lt 5 ]; do
        if "$BIN_DIR/wget" -q -O "$WORK_DIR/http_fetch.txt" "http://127.0.0.1:$HTTP_PORT/index.txt" 2> "$WORK_DIR/wget.err"; then
            fetch_status=0
            break
        fi
        if ! kill -0 "$httpd_pid" 2>/dev/null; then
            break
        fi
        "$BIN_DIR/sleep" 1
        attempt=$((attempt + 1))
    done

    if [ "$fetch_status" -ne 0 ]; then
        if ! kill -0 "$httpd_pid" 2>/dev/null && grep -q 'failed to open listener' "$WORK_DIR/httpd.log"; then
            fail "freestanding httpd failed to open listener after loopback probe succeeded"
        fi
        fail "freestanding httpd/wget round trip failed"
    fi
    assert_file_contains "$WORK_DIR/http_fetch.txt" '^hello from freestanding httpd$' "freestanding httpd/wget round trip failed"
    kill "$httpd_pid" 2>/dev/null || true
    wait "$httpd_pid" 2>/dev/null || true
    trap - EXIT INT TERM
fi

echo "FREESTANDING_SUITE_OK"
