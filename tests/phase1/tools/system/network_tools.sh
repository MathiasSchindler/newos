#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup network_tools

port=$((24000 + ($$ % 1000)))

"$ROOT_DIR/build/netcat" -4 -l -s 127.0.0.1 -w 3 "$port" > "$WORK_DIR/netcat_server.out" &
netcat_pid=$!
"$ROOT_DIR/build/sleep" 1
assert_command_succeeds "$ROOT_DIR/build/netcat" -4 -n -w 1 127.0.0.1 "$port" > "$WORK_DIR/netcat_client.out" <<'EOF'
phase1-netcat
EOF
wait "$netcat_pid"
assert_file_contains "$WORK_DIR/netcat_server.out" 'phase1-netcat' "netcat loopback mode did not deliver the payload"

assert_command_succeeds "$ROOT_DIR/build/ping" --help > "$WORK_DIR/ping_help.out" 2>&1
assert_file_contains "$WORK_DIR/ping_help.out" 'quiet output' "ping --help did not describe the extended options"

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
    dhcp_port=$((port + 100))
    dhcp_client_port=$((port + 200))
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
