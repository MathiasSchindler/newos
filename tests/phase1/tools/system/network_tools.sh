#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup network_tools

port=$((24000 + ($$ % 1000)))

"$ROOT_DIR/build/netcat" -l -w 3 "$port" > "$WORK_DIR/netcat_server.out" &
netcat_pid=$!
"$ROOT_DIR/build/sleep" 1
assert_command_succeeds "$ROOT_DIR/build/netcat" -w 1 localhost "$port" > "$WORK_DIR/netcat_client.out" <<'EOF'
phase1-netcat
EOF
wait "$netcat_pid"
assert_file_contains "$WORK_DIR/netcat_server.out" 'phase1-netcat' "netcat loopback mode did not deliver the payload"

ping_status=0
"$ROOT_DIR/build/ping" -c 1 127.0.0.1 > "$WORK_DIR/ping.out" 2>&1 || ping_status=$?
case "$ping_status" in
    0|1) ;;
    *) fail "ping returned an unexpected exit status: $ping_status" ;;
esac
assert_file_contains "$WORK_DIR/ping.out" '^PING 127\.0\.0\.1 ' "ping did not print the probe header"
assert_file_contains "$WORK_DIR/ping.out" 'ping statistics' "ping did not print the final statistics block"
