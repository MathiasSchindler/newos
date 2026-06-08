#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup network_tools

find_loopback_port() {
    protocol=${1:-tcp}
    if command -v python3 >/dev/null 2>&1; then
        python3 - <<'PY' "$protocol"
import socket
import sys

kind = socket.SOCK_DGRAM if sys.argv[1] == 'udp' else socket.SOCK_STREAM
sock = socket.socket(socket.AF_INET, kind)
try:
    sock.bind(('127.0.0.1', 0))
    print(sock.getsockname()[1])
finally:
    sock.close()
PY
        return
    fi

    port_index=$(cat "$WORK_DIR/next_port" 2>/dev/null || echo 0)
    echo $((port_index + 1)) > "$WORK_DIR/next_port"
    echo $((30000 + (($$ % 1000) * 10) + port_index))
}

port=$(find_loopback_port tcp)

"$ROOT_DIR/build/netcat" -4 -l -s 127.0.0.1 -w 3 "$port" > "$WORK_DIR/netcat_server.out" &
netcat_pid=$!
"$ROOT_DIR/build/sleep" 1
assert_command_succeeds "$ROOT_DIR/build/netcat" -4 -n -w 1 127.0.0.1 "$port" > "$WORK_DIR/netcat_client.out" <<'EOF'
phase1-netcat
EOF
wait "$netcat_pid"
assert_file_contains "$WORK_DIR/netcat_server.out" 'phase1-netcat' "netcat loopback mode did not deliver the payload"

scan_port=$(find_loopback_port tcp)
scan_closed_port=$(find_loopback_port tcp)
"$ROOT_DIR/build/netcat" -4 -l -s 127.0.0.1 -w 3 "$scan_port" > "$WORK_DIR/portscan_server.out" &
portscan_pid=$!
"$ROOT_DIR/build/sleep" 1
assert_command_succeeds "$ROOT_DIR/build/portscan" -4 -a -n -w 1s 127.0.0.1-1 "$scan_port,$scan_closed_port" > "$WORK_DIR/portscan.out" 2>&1
wait "$portscan_pid"
assert_file_contains "$WORK_DIR/portscan.out" "^127\.0\.0\.1 $scan_port open$" "portscan did not report the loopback listener as open"
assert_file_contains "$WORK_DIR/portscan.out" "^127\.0\.0\.1 $scan_closed_port closed$" "portscan did not report a closed loopback port with -a"

assert_command_succeeds "$ROOT_DIR/build/portscan" -4 -a -n --services --summary --delay 1ms -w 1s 127.0.0.1 22 > "$WORK_DIR/portscan_services.out" 2>&1
assert_file_contains "$WORK_DIR/portscan_services.out" '^127\.0\.0\.1 22 .* ssh$' "portscan --services did not show the SSH service hint"
assert_file_contains "$WORK_DIR/portscan_services.out" '^summary scanned=1 open=[01] closed=[01] filtered=[01] unreachable=[01] error=[01] elapsed_ms=[0-9][0-9]* jobs=[0-9][0-9]* timeout_ms=[0-9][0-9]*$' "portscan --summary did not print totals"

assert_command_succeeds "$ROOT_DIR/build/portscan" -4 -n 127.0.0.1 "$scan_closed_port" --summary > "$WORK_DIR/portscan_trailing_summary.out" 2>&1
assert_file_contains "$WORK_DIR/portscan_trailing_summary.out" '^summary scanned=1 open=0 closed=1 filtered=0 unreachable=0 error=0 elapsed_ms=[0-9][0-9]* jobs=[0-9][0-9]* timeout_ms=[0-9][0-9]*$' "portscan should accept --summary after host and port arguments"

assert_command_succeeds "$ROOT_DIR/build/portscan" -4 -n 127.0.0.1 --progress "$scan_closed_port" > "$WORK_DIR/portscan_progress.out" 2>&1
assert_file_contains "$WORK_DIR/portscan_progress.out" "^127\.0\.0\.1 $scan_closed_port closed$" "portscan --progress should print closed results as they complete"

assert_command_succeeds "$ROOT_DIR/build/portscan" -4 -n 127.0.0.1 --common --summary -w 1ms > "$WORK_DIR/portscan_trailing_common.out" 2>&1
assert_file_contains "$WORK_DIR/portscan_trailing_common.out" '^summary scanned=' "portscan should accept --common after the host argument"

assert_command_succeeds "$ROOT_DIR/build/portscan" -4 -a -n --csv -w 1s 127.0.0.1 "$scan_closed_port" > "$WORK_DIR/portscan_csv.out" 2>&1
assert_file_contains "$WORK_DIR/portscan_csv.out" '^host,port,state,service,latency_ms,reason,change,' "portscan --csv did not print a header"
assert_file_contains "$WORK_DIR/portscan_csv.out" "^127\.0\.0\.1,$scan_closed_port,closed,,[0-9][0-9]*,connection_refused," "portscan --csv did not print a closed row"

assert_command_succeeds "$ROOT_DIR/build/portscan" --json -4 -a -n --summary -w 1s 127.0.0.1 "$scan_closed_port" > "$WORK_DIR/portscan_json.out" 2>&1
assert_file_contains "$WORK_DIR/portscan_json.out" '"schema":"newos.tool.v1"' "portscan --json did not use the shared JSON envelope"
assert_file_contains "$WORK_DIR/portscan_json.out" '"event":"port_result"' "portscan --json did not emit port_result events"
assert_file_contains "$WORK_DIR/portscan_json.out" "\"port\":$scan_closed_port" "portscan --json did not report the scanned port"
assert_file_contains "$WORK_DIR/portscan_json.out" '"state":"closed"' "portscan --json did not report the closed state"
assert_file_contains "$WORK_DIR/portscan_json.out" '"event":"scan_summary"' "portscan --json --summary did not emit scan_summary"

portscan_csv_json_status=0
"$ROOT_DIR/build/portscan" --csv --json -4 -n 127.0.0.1 "$scan_closed_port" > "$WORK_DIR/portscan_csv_json.out" 2>&1 || portscan_csv_json_status=$?
assert_exit_code "$portscan_csv_json_status" 1 "portscan should reject --csv with --json"
assert_file_contains "$WORK_DIR/portscan_csv_json.out" '"event":"diagnostic"' "portscan --csv --json should report a JSON diagnostic"

assert_command_succeeds "$ROOT_DIR/build/portscan" -4 -a -n -w 1s not-a-numeric-host "$scan_closed_port" > "$WORK_DIR/portscan_unreachable.out" 2>&1
assert_file_contains "$WORK_DIR/portscan_unreachable.out" "^not-a-numeric-host $scan_closed_port unreachable$" "portscan did not print an unreachable state for an unresolvable numeric-only host"

portscan_fail_closed_status=0
"$ROOT_DIR/build/portscan" -4 -a -n --fail-closed -w 1s 127.0.0.1 "$scan_closed_port" > "$WORK_DIR/portscan_fail_closed.out" 2>&1 || portscan_fail_closed_status=$?
assert_exit_code "$portscan_fail_closed_status" 3 "portscan --fail-closed should return 3 when a closed port is found"

assert_command_succeeds "$ROOT_DIR/build/portscan" --help > "$WORK_DIR/portscan_help.out" 2>&1
assert_file_contains "$WORK_DIR/portscan_help.out" 'authorized hosts' "portscan --help did not describe authorized-host usage"
assert_file_contains "$WORK_DIR/portscan_help.out" 'common-port scanning' "portscan --help did not describe common-port scanning"
assert_file_contains "$WORK_DIR/portscan_help.out" 'passively read any banner' "portscan --help did not describe the banner option"

if command -v python3 >/dev/null 2>&1; then
    banner_port=$(find_loopback_port tcp)
    python3 - <<'PY' "$banner_port" > "$WORK_DIR/banner_server.out" 2>&1 &
import socket, sys
port = int(sys.argv[1])
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('127.0.0.1', port))
sock.listen(2)
for _ in range(2):
    conn, _ = sock.accept()
    try:
        conn.sendall(b'SSH-2.0-portscan-test\r\n\x01hi')
    except OSError:
        pass
    conn.close()
sock.close()
print('MOCK_BANNER_OK')
PY
    banner_server_pid=$!
    "$ROOT_DIR/build/sleep" 1
    assert_command_succeeds "$ROOT_DIR/build/portscan" -4 -n -w 1s --banner --banner-timeout 500ms 127.0.0.1 "$banner_port" > "$WORK_DIR/portscan_banner.out" 2>&1
    assert_file_contains "$WORK_DIR/portscan_banner.out" "^127\.0\.0\.1 $banner_port open SSH-2\.0-portscan-test\\\\r\\\\n\\\\x01hi$" "portscan --banner did not display the escaped banner"
    assert_command_succeeds "$ROOT_DIR/build/portscan" -4 -n -w 1s --banner --csv --banner-timeout 500ms 127.0.0.1 "$banner_port" > "$WORK_DIR/portscan_banner_csv.out" 2>&1
    assert_file_contains "$WORK_DIR/portscan_banner_csv.out" '^host,port,state,service,latency_ms,reason,change,' "portscan --banner --csv header missing banner column"
    assert_file_contains "$WORK_DIR/portscan_banner_csv.out" "^127\.0\.0\.1,$banner_port,open,,[0-9][0-9]*,connected,,SSH-2\.0-portscan-test\\\\r\\\\n\\\\x01hi" "portscan --banner --csv did not include the escaped banner"
    wait "$banner_server_pid" || true
    assert_file_contains "$WORK_DIR/banner_server.out" 'MOCK_BANNER_OK' "mock banner server did not complete"
fi

quiet_port=$(find_loopback_port tcp)
"$ROOT_DIR/build/netcat" -4 -l -s 127.0.0.1 -w 3 "$quiet_port" > "$WORK_DIR/portscan_quiet_server.out" 2>&1 &
quiet_pid=$!
"$ROOT_DIR/build/sleep" 1
assert_command_succeeds "$ROOT_DIR/build/portscan" -4 -n -w 1s --banner --banner-timeout 200ms 127.0.0.1 "$quiet_port" > "$WORK_DIR/portscan_quiet.out" 2>&1
wait "$quiet_pid" 2>/dev/null || true
assert_file_contains "$WORK_DIR/portscan_quiet.out" "^127\.0\.0\.1 $quiet_port open$" "portscan --banner on a quiet port should print the result without banner text"

portscan_banner_bytes_status=0
"$ROOT_DIR/build/portscan" --banner-bytes 0 -4 -n 127.0.0.1 1 > "$WORK_DIR/portscan_banner_bytes.out" 2>&1 || portscan_banner_bytes_status=$?
assert_exit_code "$portscan_banner_bytes_status" 1 "portscan --banner-bytes 0 should be rejected"
portscan_banner_bytes_status=0
"$ROOT_DIR/build/portscan" --banner-bytes 9999 -4 -n 127.0.0.1 1 > "$WORK_DIR/portscan_banner_bytes_big.out" 2>&1 || portscan_banner_bytes_status=$?
assert_exit_code "$portscan_banner_bytes_status" 1 "portscan --banner-bytes above the cap should be rejected"

assert_command_succeeds "$ROOT_DIR/build/ping" --help > "$WORK_DIR/ping_help.out" 2>&1
assert_file_contains "$WORK_DIR/ping_help.out" 'quiet output' "ping --help did not describe the extended options"

assert_command_succeeds "$ROOT_DIR/build/traceroute" --help > "$WORK_DIR/traceroute_help.out" 2>&1
assert_file_contains "$WORK_DIR/traceroute_help.out" 'increasing ICMP TTL' "traceroute --help did not describe TTL probing"

traceroute_bad_status=0
"$ROOT_DIR/build/traceroute" -m 0 127.0.0.1 > "$WORK_DIR/traceroute_bad.out" 2>&1 || traceroute_bad_status=$?
assert_exit_code "$traceroute_bad_status" 1 "traceroute should reject a zero max TTL"
assert_file_contains "$WORK_DIR/traceroute_bad.out" '^Usage: traceroute ' "traceroute did not print usage for an invalid max TTL"

traceroute_status=0
"$ROOT_DIR/build/traceroute" -4 -m 1 -q 1 -w 1 127.0.0.1 > "$WORK_DIR/traceroute_loopback.out" 2>&1 || traceroute_status=$?
case "$traceroute_status" in
    0|1) ;;
    *) fail "traceroute returned an unexpected exit status: $traceroute_status" ;;
esac
if grep -q '^PING ' "$WORK_DIR/traceroute_loopback.out" || grep -q 'ping statistics' "$WORK_DIR/traceroute_loopback.out"; then
    fail "traceroute should not print ping banners or ping statistics"
fi

traceroute_json_status=0
"$ROOT_DIR/build/traceroute" --json -4 -m 1 -q 1 -w 1 127.0.0.1 > "$WORK_DIR/traceroute_json.out" 2>&1 || traceroute_json_status=$?
case "$traceroute_json_status" in
    0|1) ;;
    *) fail "traceroute --json returned an unexpected exit status: $traceroute_json_status" ;;
esac
assert_file_contains "$WORK_DIR/traceroute_json.out" '"schema":"newos.tool.v1"' "traceroute --json did not use the shared JSON envelope"
assert_file_contains "$WORK_DIR/traceroute_json.out" '"event":"trace_start"' "traceroute --json did not emit trace_start"
assert_file_contains "$WORK_DIR/traceroute_json.out" '"host":"127.0.0.1"' "traceroute --json did not report the target host"

traceroute6_status=0
"$ROOT_DIR/build/traceroute" -6 -m 1 -q 1 -w 1 ::1 > "$WORK_DIR/traceroute6_loopback.out" 2>&1 || traceroute6_status=$?
case "$traceroute6_status" in
    0|1) ;;
    *) fail "traceroute -6 returned an unexpected exit status: $traceroute6_status" ;;
esac
if grep -q '^PING ' "$WORK_DIR/traceroute6_loopback.out" || grep -q 'ping statistics' "$WORK_DIR/traceroute6_loopback.out"; then
    fail "traceroute -6 should not print ping banners or ping statistics"
fi

if command -v python3 >/dev/null 2>&1; then
    whois_port=$(find_loopback_port tcp)
    python3 - <<'PY' "$whois_port" > "$WORK_DIR/whois_server.out" 2>&1 &
import socket, sys
port = int(sys.argv[1])
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('127.0.0.1', port))
sock.listen(2)
for _ in range(2):
    conn, _ = sock.accept()
    data = b''
    while not data.endswith(b'\n'):
        chunk = conn.recv(1024)
        if not chunk:
            break
        data += chunk
    conn.sendall(b'Domain Name: EXAMPLE.TEST\r\nQuery: ' + data.strip() + b'\r\n')
    conn.close()
sock.close()
print('MOCK_WHOIS_OK')
PY
    whois_server_pid=$!
    "$ROOT_DIR/build/sleep" 1
    whois_status=0
    "$ROOT_DIR/build/whois" -h 127.0.0.1 -p "$whois_port" example.test > "$WORK_DIR/whois.out" 2>&1 || whois_status=$?
    whois_json_status=0
    "$ROOT_DIR/build/whois" --json -h 127.0.0.1 -p "$whois_port" example.test > "$WORK_DIR/whois_json.out" 2>&1 || whois_json_status=$?
    if [ "$whois_status" -ne 0 ]; then
        kill "$whois_server_pid" 2>/dev/null || true
    fi
    wait "$whois_server_pid" || true
    assert_exit_code "$whois_status" 0 "whois did not complete the mock query"
    assert_exit_code "$whois_json_status" 0 "whois --json did not complete the mock query"
    assert_file_contains "$WORK_DIR/whois_server.out" 'MOCK_WHOIS_OK' "mock whois server did not complete"
    assert_file_contains "$WORK_DIR/whois.out" 'Domain Name: EXAMPLE\.TEST' "whois did not print the mock response"
    assert_file_contains "$WORK_DIR/whois.out" 'Query: example\.test' "whois did not send the requested query"
    assert_file_contains "$WORK_DIR/whois_json.out" '"event":"whois_query_start"' "whois --json did not emit whois_query_start"
    assert_file_contains "$WORK_DIR/whois_json.out" '"event":"whois_response_chunk"' "whois --json did not emit response chunks"
    assert_file_contains "$WORK_DIR/whois_json.out" '"text":"Domain Name: EXAMPLE.TEST' "whois --json did not include response text"
    assert_file_contains "$WORK_DIR/whois_json.out" '"event":"whois_query_complete"' "whois --json did not emit whois_query_complete"

    silent_whois_port=$(find_loopback_port tcp)
    python3 - <<'PY' "$silent_whois_port" > "$WORK_DIR/whois_silent_server.out" 2>&1 &
import socket, sys, time
port = int(sys.argv[1])
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('127.0.0.1', port))
sock.listen(1)
conn, _ = sock.accept()
try:
    conn.recv(1024)
    time.sleep(2)
finally:
    conn.close()
    sock.close()
print('MOCK_WHOIS_SILENT_OK')
PY
    silent_whois_pid=$!
    "$ROOT_DIR/build/sleep" 1
    whois_timeout_status=0
    "$ROOT_DIR/build/whois" -w 1 -h 127.0.0.1 -p "$silent_whois_port" example.test > "$WORK_DIR/whois_timeout.out" 2>&1 || whois_timeout_status=$?
    wait "$silent_whois_pid" || true
    assert_exit_code "$whois_timeout_status" 1 "whois should fail instead of hanging on a silent server"
    assert_file_contains "$WORK_DIR/whois_timeout.out" 'read timeout from 127\.0\.0\.1' "whois did not report the silent server timeout"
    assert_file_contains "$WORK_DIR/whois_silent_server.out" 'MOCK_WHOIS_SILENT_OK' "mock silent whois server did not complete"
fi

lookup_status=0
"$ROOT_DIR/build/nslookup" localhost > "$WORK_DIR/nslookup.out" 2>&1 || lookup_status=$?
assert_exit_code "$lookup_status" 0 "nslookup localhost should succeed"
assert_file_contains "$WORK_DIR/nslookup.out" '^Name:[[:space:]]*localhost$' "nslookup did not print the queried name"
assert_file_contains "$WORK_DIR/nslookup.out" '127\.0\.0\.1' "nslookup did not report a loopback address"

assert_command_succeeds "$ROOT_DIR/build/dig" --help > "$WORK_DIR/dig_help.out" 2>&1
assert_file_contains "$WORK_DIR/dig_help.out" 'A, AAAA, MX, NS, TXT' "dig --help did not describe the supported record types"

dig_status=0
"$ROOT_DIR/build/dig" localhost > "$WORK_DIR/dig.out" 2>&1 || dig_status=$?
assert_exit_code "$dig_status" 0 "dig localhost should succeed"
assert_file_contains "$WORK_DIR/dig.out" '^;; QUESTION SECTION:$' "dig did not print a question section"
assert_file_contains "$WORK_DIR/dig.out" '^;; ANSWER SECTION:$' "dig did not print an answer section"
assert_file_contains "$WORK_DIR/dig.out" '127\.0\.0\.1' "dig did not report the IPv4 localhost answer"

dig6_status=0
"$ROOT_DIR/build/dig" -t AAAA localhost > "$WORK_DIR/dig_aaaa.out" 2>&1 || dig6_status=$?
assert_exit_code "$dig6_status" 0 "dig AAAA localhost should succeed"
assert_file_contains "$WORK_DIR/dig_aaaa.out" '^; <<>> dig <<>> localhost AAAA$' "dig -t AAAA did not show the requested record type"
assert_file_contains "$WORK_DIR/dig_aaaa.out" '::1' "dig -t AAAA did not report the IPv6 loopback answer"

dig_bad_status=0
"$ROOT_DIR/build/dig" -t BOGUS localhost > "$WORK_DIR/dig_bad.out" 2>&1 || dig_bad_status=$?
assert_exit_code "$dig_bad_status" 1 "dig should reject unsupported record types"
assert_file_contains "$WORK_DIR/dig_bad.out" 'unsupported type' "dig did not report an unsupported record type error"

if command -v python3 >/dev/null 2>&1; then
    malformed_dns_port_file="$WORK_DIR/dig_malformed_dns_port.txt"
    python3 - <<'PY' "$malformed_dns_port_file" > "$WORK_DIR/dig_malformed_dns_server.out" 2>&1 &
import socket
import sys

port_file = sys.argv[1]
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", 0))
sock.settimeout(5.0)
with open(port_file, "w", encoding="utf-8") as handle:
    handle.write(str(sock.getsockname()[1]) + "\n")
data, addr = sock.recvfrom(512)
end = 12
while end < len(data) and data[end] != 0:
    end += data[end] + 1
question = data[12:end + 5]
answer_name = bytes([0x40]) + b"a" * 64 + b"\x00"
answer = answer_name + b"\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04\x7f\x00\x00\x01"
response = data[:2] + b"\x81\x80\x00\x01\x00\x01\x00\x00\x00\x00" + question + answer
sock.sendto(response, addr)
sock.close()
print("MOCK_DNS_MALFORMED_OK")
PY
    malformed_dns_pid=$!
    waits=0
    while [ ! -s "$malformed_dns_port_file" ] && [ "$waits" -lt 5 ]; do
        "$ROOT_DIR/build/sleep" 1
        waits=$((waits + 1))
    done
    [ -s "$malformed_dns_port_file" ] || fail "mock malformed DNS server did not publish a port"
    malformed_dns_port=$(cat "$malformed_dns_port_file" | tr -d ' \r\n')
    dig_malformed_status=0
    "$ROOT_DIR/build/dig" -s 127.0.0.1 -p "$malformed_dns_port" malformed.test > "$WORK_DIR/dig_malformed_dns.out" 2>&1 || dig_malformed_status=$?
    wait "$malformed_dns_pid" || true
    assert_exit_code "$dig_malformed_status" 1 "dig should reject DNS names with reserved label types"
    assert_file_contains "$WORK_DIR/dig_malformed_dns.out" 'lookup failed for malformed\.test' "dig did not report the malformed DNS lookup failure"
    assert_file_contains "$WORK_DIR/dig_malformed_dns_server.out" 'MOCK_DNS_MALFORMED_OK' "mock malformed DNS server did not complete"
else
    note "python3 not available; skipping malformed DNS reply test"
fi

assert_command_succeeds "$ROOT_DIR/build/ping6" --help > "$WORK_DIR/ping6_help.out" 2>&1
assert_file_contains "$WORK_DIR/ping6_help.out" 'IPv6' "ping6 --help did not describe IPv6 probing"

ping_ipv6_status=0
"$ROOT_DIR/build/ping" -6 ::1 > "$WORK_DIR/ping_ipv6.out" 2>&1 || ping_ipv6_status=$?
case "$ping_ipv6_status" in
    0|1) ;;
    *) fail "ping -6 returned an unexpected exit status: $ping_ipv6_status" ;;
esac
assert_file_contains "$WORK_DIR/ping_ipv6.out" '^PING ::1 ' "ping -6 did not print the IPv6 probe header"
assert_file_contains "$WORK_DIR/ping_ipv6.out" 'ping statistics' "ping -6 did not print the final statistics block"

ping6_status=0
"$ROOT_DIR/build/ping6" ::1 > "$WORK_DIR/ping6.out" 2>&1 || ping6_status=$?
case "$ping6_status" in
    0|1) ;;
    *) fail "ping6 returned an unexpected exit status: $ping6_status" ;;
esac
assert_file_contains "$WORK_DIR/ping6.out" '^PING ::1 ' "ping6 did not behave like an IPv6 ping wrapper"

assert_command_succeeds "$ROOT_DIR/build/dhcp" --help > "$WORK_DIR/dhcp_help.out" 2>&1
assert_file_contains "$WORK_DIR/dhcp_help.out" 'DHCP' "dhcp --help did not describe lease acquisition"

if command -v python3 >/dev/null 2>&1; then
    dhcp_port=$(find_loopback_port udp)
    dhcp_client_port=$(find_loopback_port udp)
    python3 - <<'PY' "$dhcp_port" > "$WORK_DIR/dhcp_server.out" 2>&1 &
import socket, sys
port = int(sys.argv[1])
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(('127.0.0.1', port))
for _ in range(2):
    data, addr = sock.recvfrom(2048)
    xid = data[4:8]
    chaddr = data[28:34]
    msg_type = 2
    i = 240
    while i + 1 < len(data):
        opt = data[i]
        if opt == 255:
            break
        if opt == 0:
            i += 1
            continue
        ln = data[i + 1]
        if opt == 53 and ln >= 1:
            msg_type = 5 if data[i + 2] == 3 else 2
            break
        i += 2 + ln
    pkt = bytearray(300)
    pkt[0] = 2
    pkt[1] = 1
    pkt[2] = 6
    pkt[4:8] = xid
    pkt[16:20] = bytes([10, 55, 0, 42])
    pkt[28:34] = chaddr
    pkt[236:240] = b'\x63\x82\x53\x63'
    opts = bytes([
        53, 1, msg_type,
        54, 4, 127, 0, 0, 1,
        1, 4, 255, 255, 255, 0,
        3, 4, 10, 55, 0, 1,
        6, 8, 10, 55, 0, 1, 10, 55, 0, 2,
        51, 4, 0, 0, 1, 44,
        255,
    ])
    pkt[240:240+len(opts)] = opts
    sock.sendto(pkt[:240+len(opts)], addr)
sock.close()
print('MOCK_DHCP_OK')
PY
    dhcp_server_pid=$!
    "$ROOT_DIR/build/sleep" 1
    dhcp_status=0
    "$ROOT_DIR/build/dhcp" -s 127.0.0.1 -p "$dhcp_port" -P "$dhcp_client_port" -t 2s > "$WORK_DIR/dhcp.out" 2>&1 || dhcp_status=$?
    if [ "$dhcp_status" -ne 0 ]; then
        kill "$dhcp_server_pid" 2>/dev/null || true
    fi
    wait "$dhcp_server_pid" || true
    assert_exit_code "$dhcp_status" 0 "dhcp did not complete the mock lease exchange"
    assert_file_contains "$WORK_DIR/dhcp_server.out" 'MOCK_DHCP_OK' "mock DHCP server did not complete the exchange"
    assert_file_contains "$WORK_DIR/dhcp.out" '^DHCP lease acquired$' "dhcp did not report lease acquisition"
    assert_file_contains "$WORK_DIR/dhcp.out" '10\.55\.0\.42/24' "dhcp did not print the leased address"
fi

ping_status=0
"$ROOT_DIR/build/ping" -c 1 127.0.0.1 > "$WORK_DIR/ping.out" 2>&1 || ping_status=$?
case "$ping_status" in
    0|1) ;;
    *) fail "ping returned an unexpected exit status: $ping_status" ;;
esac
assert_file_contains "$WORK_DIR/ping.out" '^PING 127\.0\.0\.1 ' "ping did not print the probe header"
assert_file_contains "$WORK_DIR/ping.out" 'ping statistics' "ping did not print the final statistics block"
