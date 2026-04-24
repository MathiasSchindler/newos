#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"

BIN_DIR=${NEWOS_FREESTANDING_BUILD_DIR:-"$ROOT_DIR/build/freestanding-linux-$(uname -m)"}
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

if command -v readelf >/dev/null 2>&1; then
    LC_ALL=C readelf -h "$BIN_DIR/httpd" > "$WORK_DIR/httpd.elf"
    assert_file_contains "$WORK_DIR/httpd.elf" 'Type:[[:space:]]*DYN' "freestanding httpd should be a static PIE"
    LC_ALL=C readelf -lW "$BIN_DIR/httpd" > "$WORK_DIR/httpd.phdr"
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
        if "$BIN_DIR/wget" -q -O "$WORK_DIR/http_fetch.txt" "http://127.0.0.1:$HTTP_PORT/index.txt"; then
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
