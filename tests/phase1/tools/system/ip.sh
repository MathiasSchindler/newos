#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup ip

assert_command_succeeds "$ROOT_DIR/build/ip" --help > "$WORK_DIR/help.out"
assert_file_contains "$WORK_DIR/help.out" '^Usage: ' "ip --help did not print a usage banner"

assert_command_succeeds "$ROOT_DIR/build/ip" addr > "$WORK_DIR/addr.out"
assert_file_contains "$WORK_DIR/addr.out" '^[0-9][0-9]*: ' "ip addr did not print any interface headers"
assert_file_contains "$WORK_DIR/addr.out" 'inet ' "ip addr did not report an IPv4 address"

dev_name=$(sed -n 's/^[0-9][0-9]*: \([^: ][^:]*\):.*/\1/p' "$WORK_DIR/addr.out" | head -n 1)
assert_nonempty_text "$dev_name" "ip addr output did not expose a device name"

assert_command_succeeds "$ROOT_DIR/build/ip" link show dev "$dev_name" > "$WORK_DIR/link.out"
assert_file_contains "$WORK_DIR/link.out" ": $dev_name: <" "ip link show dev did not print the selected device"
assert_file_contains "$WORK_DIR/link.out" ' mtu ' "ip link did not show MTU information"

assert_command_succeeds "$ROOT_DIR/build/ip" addr show "$dev_name" > "$WORK_DIR/addr_dev.out"
assert_file_contains "$WORK_DIR/addr_dev.out" ": $dev_name: <" "ip addr show IFACE did not accept the short compatibility form"

assert_command_succeeds "$ROOT_DIR/build/ip" -4 addr show > "$WORK_DIR/ipv4.out"
assert_file_contains "$WORK_DIR/ipv4.out" 'inet ' "ip -4 addr show did not keep IPv4 output"

assert_command_succeeds "$ROOT_DIR/build/ip" -br addr show > "$WORK_DIR/brief.out"
assert_file_contains "$WORK_DIR/brief.out" "^$dev_name" "ip -br addr show did not print the brief summary format"
assert_file_contains "$WORK_DIR/brief.out" '/[0-9][0-9]*' "ip -br addr show did not include any prefix-length addresses"

assert_command_succeeds "$ROOT_DIR/build/ip" route > "$WORK_DIR/route.out"
[ -s "$WORK_DIR/route.out" ] || fail "ip route succeeded without printing any route data"
assert_file_contains "$WORK_DIR/route.out" ' dev ' "ip route did not print recognizable route entries"

assert_command_succeeds "$ROOT_DIR/build/ip" route show dev "$dev_name" > "$WORK_DIR/route_dev.out"
if [ -s "$WORK_DIR/route_dev.out" ]; then
    assert_file_contains "$WORK_DIR/route_dev.out" " dev $dev_name" "ip route show dev did not preserve the device filter"
fi

assert_command_succeeds "$ROOT_DIR/build/ip" -6 route show > "$WORK_DIR/route6.out"
if [ -s "$WORK_DIR/route6.out" ]; then
    assert_file_contains "$WORK_DIR/route6.out" ':' "ip -6 route show did not produce IPv6-style route output"
fi

if grep -q '^default' "$WORK_DIR/route.out"; then
    assert_command_succeeds "$ROOT_DIR/build/ip" route show default > "$WORK_DIR/default.out"
    assert_file_contains "$WORK_DIR/default.out" '^default' "ip route show default did not keep the default-route filter"
fi
