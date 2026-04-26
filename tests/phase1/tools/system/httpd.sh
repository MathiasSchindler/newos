#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup httpd

mkdir -p "$WORK_DIR/root"
printf 'index\n' > "$WORK_DIR/root/index.txt"

"$ROOT_DIR/build/httpd" --help > "$WORK_DIR/help.out" 2>&1
assert_file_contains "$WORK_DIR/help.out" '^Usage: ' "httpd --help did not print usage"

cat > "$WORK_DIR/httpd.conf" <<EOF
bind=127.0.0.1
port=28080
root=$WORK_DIR/root
EOF

conflict_status=0
"$ROOT_DIR/build/httpd" -p 28081 -c "$WORK_DIR/httpd.conf" > "$WORK_DIR/conflict.out" 2>&1 || conflict_status=$?
[ "$conflict_status" -ne 0 ] || fail "httpd should reject conflicting CLI and config values"
assert_file_contains "$WORK_DIR/conflict.out" 'conflict' "httpd config conflict diagnostic did not mention the conflict"