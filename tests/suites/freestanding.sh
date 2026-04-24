#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

BIN_DIR=${NEWOS_FREESTANDING_BUILD_DIR:-"$ROOT_DIR/build/freestanding-linux-$(uname -m)"}
WORK_DIR="$ROOT_DIR/tests/tmp/freestanding"
HTTP_PORT=$((31000 + ($$ % 1000)))

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

note "freestanding binaries"

if [ ! -x "$BIN_DIR/sh" ] || [ ! -x "$BIN_DIR/httpd" ]; then
    fail "freestanding build directory is missing expected tools: $BIN_DIR"
fi

if command -v readelf >/dev/null 2>&1; then
    readelf -h "$BIN_DIR/httpd" > "$WORK_DIR/httpd.elf"
    assert_file_contains "$WORK_DIR/httpd.elf" 'Type:[[:space:]]*DYN' "freestanding httpd should be a static PIE"
    readelf -lW "$BIN_DIR/httpd" > "$WORK_DIR/httpd.phdr"
    assert_file_contains "$WORK_DIR/httpd.phdr" 'GNU_STACK[[:space:]].* RW ' "freestanding httpd stack should be non-executable"
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

cat > "$WORK_DIR/script.sh" <<EOF
$BIN_DIR/echo shell-ok
$BIN_DIR/printf '%s\\n' pipe-ok | $BIN_DIR/tr a-z A-Z
EOF
"$BIN_DIR/sh" "$WORK_DIR/script.sh" > "$WORK_DIR/sh.out"
assert_file_contains "$WORK_DIR/sh.out" '^shell-ok$' "freestanding shell did not run an absolute command"
assert_file_contains "$WORK_DIR/sh.out" '^PIPE-OK$' "freestanding shell pipeline failed"

mkdir -p "$WORK_DIR/http_root"
printf 'hello from freestanding httpd\n' > "$WORK_DIR/http_root/index.txt"
"$BIN_DIR/httpd" -p "$HTTP_PORT" -r "$WORK_DIR/http_root" -i index.txt > "$WORK_DIR/httpd.log" 2>&1 &
httpd_pid=$!
trap 'kill "$httpd_pid" 2>/dev/null || true' EXIT INT TERM
"$BIN_DIR/sleep" 1
"$BIN_DIR/wget" -q -O "$WORK_DIR/http_fetch.txt" "http://127.0.0.1:$HTTP_PORT/index.txt"
assert_file_contains "$WORK_DIR/http_fetch.txt" '^hello from freestanding httpd$' "freestanding httpd/wget round trip failed"
kill "$httpd_pid" 2>/dev/null || true
wait "$httpd_pid" 2>/dev/null || true
trap - EXIT INT TERM

echo "FREESTANDING_SUITE_OK"
