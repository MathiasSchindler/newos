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

assert_command_succeeds "$ROOT_DIR/build/ping" --help > "$WORK_DIR/ping_help.out"
assert_file_contains "$WORK_DIR/ping_help.out" 'quiet output' "ping --help did not describe the extended options"

ping_ipv6_status=0
"$ROOT_DIR/build/ping" -6 ::1 > "$WORK_DIR/ping_ipv6.out" 2>&1 || ping_ipv6_status=$?
assert_exit_code "$ping_ipv6_status" 1 "ping -6 should fail cleanly until IPv6 probing is implemented"
assert_file_contains "$WORK_DIR/ping_ipv6.out" 'IPv6 echo requests are not yet implemented' "ping -6 did not explain the current IPv6 limitation"

ping_status=0
"$ROOT_DIR/build/ping" -c 1 127.0.0.1 > "$WORK_DIR/ping.out" 2>&1 || ping_status=$?
case "$ping_status" in
    0|1) ;;
    *) fail "ping returned an unexpected exit status: $ping_status" ;;
esac
assert_file_contains "$WORK_DIR/ping.out" '^PING 127\.0\.0\.1 ' "ping did not print the probe header"
assert_file_contains "$WORK_DIR/ping.out" 'ping statistics' "ping did not print the final statistics block"
