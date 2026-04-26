#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup service

cat > "$WORK_DIR/service.conf" <<EOF
command=/bin/sleep 1
pidfile=$WORK_DIR/service.pid
stdout=$WORK_DIR/service.out
stderr=$WORK_DIR/service.err
stop_timeout=1s
EOF

service_pid=

cleanup_service() {
    if [ -n "$service_pid" ]; then
        "$ROOT_DIR/build/service" stop "$WORK_DIR/service.conf" >/dev/null 2>&1 || true
        kill "$service_pid" 2>/dev/null || true
    fi
}
trap 'cleanup_service' EXIT HUP INT TERM

"$ROOT_DIR/build/service" start "$WORK_DIR/service.conf" > "$WORK_DIR/start.out"
assert_file_contains "$WORK_DIR/start.out" 'started' "service start did not report startup"
service_pid=$(sed -n 's/^pid=//p' "$WORK_DIR/service.pid")
[ -n "$service_pid" ] || fail "service start did not write a pidfile"
"$ROOT_DIR/build/service" stop "$WORK_DIR/service.conf" > "$WORK_DIR/stop.out" || true
assert_file_contains "$WORK_DIR/stop.out" 'stopped' "service stop did not report shutdown"
kill "$service_pid" 2>/dev/null || true
service_pid=
trap - EXIT HUP INT TERM

cat > "$WORK_DIR/unsafe.conf" <<'EOF'
command=sleep 1
pidfile=/tmp/newos-phase1-unsafe.pid
EOF
unsafe_status=0
"$ROOT_DIR/build/service" start "$WORK_DIR/unsafe.conf" > "$WORK_DIR/unsafe.out" 2>&1 || unsafe_status=$?
[ "$unsafe_status" -ne 0 ] || fail "service should reject commands without an explicit path"
assert_file_contains "$WORK_DIR/unsafe.out" 'failed to start service' "service unsafe command diagnostic was not clear"