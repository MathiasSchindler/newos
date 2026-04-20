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
assert_file_contains "$WORK_DIR/nslookup.out" '127\.0\.0\.1|::1' "nslookup did not report a loopback address"

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

ping_status=0
"$ROOT_DIR/build/ping" -c 1 127.0.0.1 > "$WORK_DIR/ping.out" 2>&1 || ping_status=$?
case "$ping_status" in
    0|1) ;;
    *) fail "ping returned an unexpected exit status: $ping_status" ;;
esac
assert_file_contains "$WORK_DIR/ping.out" '^PING 127\.0\.0\.1 ' "ping did not print the probe header"
assert_file_contains "$WORK_DIR/ping.out" 'ping statistics' "ping did not print the final statistics block"
