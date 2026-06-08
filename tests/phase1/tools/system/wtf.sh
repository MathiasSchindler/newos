#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup wtf

if command -v python3 >/dev/null 2>&1; then
    port_file="$WORK_DIR/wtf_port.txt"
    python3 - <<'PY' "$port_file" > "$WORK_DIR/wtf_server.out" 2>&1 &
import json
import socket
import sys

port_file = sys.argv[1]
body = json.dumps({
    "title": "Ada Lovelace",
    "description": "English mathematician and writer",
    "extract": "Ada Lovelace wrote notes on the Analytical Engine.",
    "page": "https://example.test/wiki/Ada_Lovelace",
}).encode("utf-8")

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("127.0.0.1", 0))
sock.listen(1)
with open(port_file, "w", encoding="utf-8") as handle:
    handle.write(str(sock.getsockname()[1]) + "\n")

conn, _ = sock.accept()
conn.recv(4096)
response = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Type: application/json\r\n"
    + b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n"
    + b"Connection: close\r\n\r\n"
    + body
)
conn.sendall(response)
conn.close()
sock.close()
print("MOCK_WTF_OK")
PY
    wtf_server_pid=$!
    waits=0
    while [ ! -s "$port_file" ] && [ "$waits" -lt 5 ]; do
        "$ROOT_DIR/build/sleep" 1
        waits=$((waits + 1))
    done
    [ -s "$port_file" ] || fail "wtf mock server did not publish a port"
    wtf_port=$(cat "$port_file" | tr -d ' \r\n')
    wtf_status=0
    "$ROOT_DIR/build/wtf" --json --base-url "http://127.0.0.1:$wtf_port/api/rest_v1/page/summary" Ada Lovelace > "$WORK_DIR/wtf_json.out" 2>&1 || wtf_status=$?
    wait "$wtf_server_pid" || true
    assert_exit_code "$wtf_status" 0 "wtf --json did not complete against the mock endpoint"
    assert_file_contains "$WORK_DIR/wtf_server.out" 'MOCK_WTF_OK' "mock wtf server did not complete"
    assert_file_contains "$WORK_DIR/wtf_json.out" '"schema":"newos.tool.v1"' "wtf --json did not use the shared JSON envelope"
    assert_file_contains "$WORK_DIR/wtf_json.out" '"event":"wtf_summary"' "wtf --json did not emit wtf_summary"
    assert_file_contains "$WORK_DIR/wtf_json.out" '"term":"Ada Lovelace"' "wtf --json did not report the lookup term"
    assert_file_contains "$WORK_DIR/wtf_json.out" '"title":"Ada Lovelace"' "wtf --json did not report the title"
    assert_file_contains "$WORK_DIR/wtf_json.out" '"page_url":"https://example.test/wiki/Ada_Lovelace"' "wtf --json did not report the page URL"

    port_file="$WORK_DIR/wtf_length_port.txt"
    python3 - <<'PY' "$port_file" > "$WORK_DIR/wtf_length_server.out" 2>&1 &
import json
import socket
import sys

port_file = sys.argv[1]
body = json.dumps({
    "title": "Length Test",
    "description": "fixture",
    "extract": "Content length should be enough.",
}).encode("utf-8")

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("127.0.0.1", 0))
sock.listen(1)
with open(port_file, "w", encoding="utf-8") as handle:
    handle.write(str(sock.getsockname()[1]) + "\n")

conn, _ = sock.accept()
conn.recv(4096)
response = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Type: application/json\r\n"
    + b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n\r\n"
    + body
)
conn.sendall(response)
conn.settimeout(0.25)
try:
    data = conn.recv(1)
    if data == b"":
        print("CLIENT_CLOSED_AFTER_LENGTH")
    else:
        print("CLIENT_SENT_EXTRA_DATA")
except (socket.timeout, TimeoutError):
    print("CLIENT_WAITED_FOR_CLOSE")
except ConnectionResetError:
    print("CLIENT_CLOSED_AFTER_LENGTH")
conn.close()
sock.close()
PY
    wtf_length_server_pid=$!
    waits=0
    while [ ! -s "$port_file" ] && [ "$waits" -lt 5 ]; do
        "$ROOT_DIR/build/sleep" 1
        waits=$((waits + 1))
    done
    [ -s "$port_file" ] || fail "wtf content-length mock server did not publish a port"
    wtf_length_port=$(cat "$port_file" | tr -d ' \r\n')
    wtf_length_status=0
    "$ROOT_DIR/build/wtf" --base-url "http://127.0.0.1:$wtf_length_port/api/rest_v1/page/summary" --only-title Length Test > "$WORK_DIR/wtf_length.out" 2>&1 || wtf_length_status=$?
    wait "$wtf_length_server_pid" || true
    assert_exit_code "$wtf_length_status" 0 "wtf did not complete against content-length endpoint"
    assert_file_contains "$WORK_DIR/wtf_length.out" '^Length Test$' "wtf did not parse the content-length response"
    assert_file_contains "$WORK_DIR/wtf_length_server.out" 'CLIENT_CLOSED_AFTER_LENGTH' "wtf should stop reading after Content-Length bytes"
else
    note "python3 not available; skipping wtf mock HTTP JSON test"
fi