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

    port_file="$WORK_DIR/wtf_control_port.txt"
    python3 - <<'PY' "$port_file" > "$WORK_DIR/wtf_control_server.out" 2>&1 &
import socket
import sys

port_file = sys.argv[1]
responses = [
    b'{"title":"Control","description":"fixture","extract":"safe \\u001b[31m text"}',
    b'{"title":"Broken","description":"fixture","extract":"bad \\u12"}',
]

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("127.0.0.1", 0))
sock.listen(2)
with open(port_file, "w", encoding="utf-8") as handle:
    handle.write(str(sock.getsockname()[1]) + "\n")

for body in responses:
    conn, _ = sock.accept()
    conn.recv(4096)
    conn.sendall(
        b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
        + str(len(body)).encode("ascii")
        + b"\r\n\r\n"
        + body
    )
    conn.close()
sock.close()
print("MOCK_WTF_CONTROL_OK")
PY
    wtf_control_server_pid=$!
    waits=0
    while [ ! -s "$port_file" ] && [ "$waits" -lt 5 ]; do
        "$ROOT_DIR/build/sleep" 1
        waits=$((waits + 1))
    done
    [ -s "$port_file" ] || fail "wtf control mock server did not publish a port"
    wtf_control_port=$(cat "$port_file" | tr -d ' \r\n')
    assert_command_succeeds "$ROOT_DIR/build/wtf" --base-url "http://127.0.0.1:$wtf_control_port/api/rest_v1/page/summary" --only-extract Control > "$WORK_DIR/wtf_control.out" 2>&1
    assert_file_contains "$WORK_DIR/wtf_control.out" 'safe  \[31m text' "wtf should sanitize decoded terminal control characters"
    python3 - <<'PY' "$WORK_DIR/wtf_control.out"
import sys
data = open(sys.argv[1], "rb").read()
if b"\x1b" in data:
    raise SystemExit("wtf emitted a raw escape byte")
PY
    wtf_broken_status=0
    "$ROOT_DIR/build/wtf" --base-url "http://127.0.0.1:$wtf_control_port/api/rest_v1/page/summary" --only-extract Broken > "$WORK_DIR/wtf_broken.out" 2>&1 || wtf_broken_status=$?
    wait "$wtf_control_server_pid" || true
    assert_exit_code "$wtf_broken_status" 1 "wtf should reject truncated JSON unicode escapes without crashing"
    assert_file_contains "$WORK_DIR/wtf_broken.out" 'could not parse summary' "wtf did not report the malformed JSON summary"
    assert_file_contains "$WORK_DIR/wtf_control_server.out" 'MOCK_WTF_CONTROL_OK' "mock wtf control server did not complete"
else
    note "python3 not available; skipping wtf mock HTTP JSON test"
fi