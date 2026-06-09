#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup wget

printf 'wget sample\n' > "$WORK_DIR/source.txt"
assert_command_succeeds "${TEST_BIN_DIR}/wget" -q -O "$WORK_DIR/copy.txt" "file://$WORK_DIR/source.txt"
assert_files_equal "$WORK_DIR/source.txt" "$WORK_DIR/copy.txt" "wget file:// download failed"

wget_stdout=$("${TEST_BIN_DIR}/wget" -q -O - "file://$WORK_DIR/source.txt" | tr -d '\r\n')
assert_text_equals "$wget_stdout" 'wget sample' "wget -O - did not stream the fetched content"

redirect_pid=
redirect_port_file="$WORK_DIR/redirect_port.txt"
printf 'redirect secret\n' > "$WORK_DIR/redirect_secret.txt"
cat > "$WORK_DIR/redirect_server.c" <<'EOF'
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int listen_fd;
    int client_fd;
    int opt = 1;
    struct sockaddr_in addr;
    socklen_t addr_len;
    FILE *port_file;
    char response[2048];

    if (argc != 3) {
        return 2;
    }
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return 3;
    }
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        close(listen_fd);
        return 4;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(listen_fd);
        return 5;
    }
    if (listen(listen_fd, 1) != 0) {
        close(listen_fd);
        return 6;
    }
    addr_len = sizeof(addr);
    if (getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        close(listen_fd);
        return 8;
    }
    port_file = fopen(argv[1], "w");
    if (port_file == NULL) {
        close(listen_fd);
        return 9;
    }
    fprintf(port_file, "%u\n", (unsigned int)ntohs(addr.sin_port));
    fclose(port_file);
    client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        close(listen_fd);
        return 7;
    }
    snprintf(response,
             sizeof(response),
             "HTTP/1.1 302 Found\r\nLocation: file://%s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
             argv[2]);
    if (write(client_fd, response, strlen(response)) < 0) {
        close(client_fd);
        close(listen_fd);
        return 10;
    }
    close(client_fd);
    close(listen_fd);
    return 0;
}
EOF
"${CC:-cc}" -O2 "$WORK_DIR/redirect_server.c" -o "$WORK_DIR/redirect_server"

cleanup_redirect_server() {
    if [ -n "${redirect_pid:-}" ]; then
        kill "$redirect_pid" 2>/dev/null || true
        wait "$redirect_pid" 2>/dev/null || true
        redirect_pid=
    fi
}

trap 'cleanup_redirect_server' EXIT HUP INT TERM
"$WORK_DIR/redirect_server" "$redirect_port_file" "$WORK_DIR/redirect_secret.txt" > "$WORK_DIR/redirect_server.out" 2>&1 &
redirect_pid=$!
redirect_waits=0
while [ ! -s "$redirect_port_file" ] && [ "$redirect_waits" -lt 5 ]; do
    "${TEST_BIN_DIR}/sleep" 1
    redirect_waits=$((redirect_waits + 1))
done
[ -s "$redirect_port_file" ] || fail "redirect test server did not publish a port"
redirect_port=$(cat "$redirect_port_file" | tr -d ' \r\n')
redirect_status=0
"${TEST_BIN_DIR}/wget" -q -T 2s -O "$WORK_DIR/redirect_copy.txt" "http://127.0.0.1:$redirect_port/redirect" > "$WORK_DIR/redirect_fetch.out" 2>&1 || redirect_status=$?
cleanup_redirect_server
trap - EXIT HUP INT TERM
if [ "$redirect_status" -eq 0 ]; then
    fail "wget should reject redirects from HTTP to file:// URLs"
fi
if [ -s "$WORK_DIR/redirect_copy.txt" ]; then
    fail "wget should not copy local file contents after a redirect"
fi
assert_file_contains "$WORK_DIR/redirect_fetch.out" 'cannot follow redirect' "wget did not report the rejected redirect"

if command -v python3 >/dev/null 2>&1; then
    malformed_port_file="$WORK_DIR/wget_malformed_port.txt"
    python3 - <<'PY' "$malformed_port_file" > "$WORK_DIR/wget_malformed_server.out" 2>&1 &
import socket
import sys

port_file = sys.argv[1]

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("127.0.0.1", 0))
sock.listen(3)
with open(port_file, "w", encoding="utf-8") as handle:
    handle.write(str(sock.getsockname()[1]) + "\n")

for _ in range(3):
    conn, _ = sock.accept()
    request = conn.recv(4096)
    if b"/length" in request:
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA")
        conn.settimeout(0.25)
        try:
            data = conn.recv(1)
            if data == b"":
                print("WGET_CLOSED_AFTER_LENGTH")
            else:
                print("WGET_SENT_EXTRA_DATA")
        except (socket.timeout, TimeoutError):
            print("WGET_WAITED_FOR_CLOSE")
        except ConnectionResetError:
            print("WGET_CLOSED_AFTER_LENGTH")
    elif b"/duplicate-length" in request:
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nxx")
    elif b"/bad-redirect" in request:
        conn.sendall(b"HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1/bad path\r\nContent-Length: 0\r\n\r\n")
    conn.close()
sock.close()
print("MOCK_WGET_MALFORMED_OK")
PY
    malformed_pid=$!
    waits=0
    while [ ! -s "$malformed_port_file" ] && [ "$waits" -lt 5 ]; do
        "${TEST_BIN_DIR}/sleep" 1
        waits=$((waits + 1))
    done
    [ -s "$malformed_port_file" ] || fail "wget malformed mock server did not publish a port"
    malformed_port=$(cat "$malformed_port_file" | tr -d ' \r\n')

    assert_command_succeeds "${TEST_BIN_DIR}/wget" -q -O "$WORK_DIR/wget_length.out" "http://127.0.0.1:$malformed_port/length"
    assert_text_equals "$(cat "$WORK_DIR/wget_length.out")" 'hello' "wget should write exactly Content-Length bytes"

    wget_duplicate_status=0
    "${TEST_BIN_DIR}/wget" -q -O "$WORK_DIR/wget_duplicate.out" "http://127.0.0.1:$malformed_port/duplicate-length" > "$WORK_DIR/wget_duplicate.err" 2>&1 || wget_duplicate_status=$?
    assert_exit_code "$wget_duplicate_status" 1 "wget should reject conflicting Content-Length headers"
    if [ -s "$WORK_DIR/wget_duplicate.out" ]; then
        fail "wget should not write a body after conflicting Content-Length headers"
    fi

    wget_bad_redirect_status=0
    "${TEST_BIN_DIR}/wget" -q -O "$WORK_DIR/wget_bad_redirect.out" "http://127.0.0.1:$malformed_port/bad-redirect" > "$WORK_DIR/wget_bad_redirect.err" 2>&1 || wget_bad_redirect_status=$?
    assert_exit_code "$wget_bad_redirect_status" 1 "wget should reject redirects containing spaces"
    if [ -s "$WORK_DIR/wget_bad_redirect.out" ]; then
        fail "wget should not write a body after a malformed redirect"
    fi

    wait "$malformed_pid" || true
    assert_file_contains "$WORK_DIR/wget_malformed_server.out" 'WGET_CLOSED_AFTER_LENGTH' "wget should close after declared Content-Length bytes"
    assert_file_contains "$WORK_DIR/wget_malformed_server.out" 'MOCK_WGET_MALFORMED_OK' "mock wget malformed server did not complete"
else
    note "python3 not available; skipping malformed wget HTTP tests"
fi
