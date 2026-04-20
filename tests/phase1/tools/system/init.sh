#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup init

assert_command_succeeds "$ROOT_DIR/build/init" -n -q "$ROOT_DIR/build/true"

init_status=0
"$ROOT_DIR/build/init" -n -q -c 'exit 7' >/dev/null 2>&1 || init_status=$?
assert_exit_code "$init_status" '7' "init did not propagate the child exit status in no-respawn mode"

: > "$WORK_DIR/init_console.log"
init_status=0
"$ROOT_DIR/build/init" -q -m 2 -t "$WORK_DIR/init_console.log" -e BOOT_STAGE=rescue -c 'printf "%s\n" "$BOOT_STAGE"; exit 1' >/dev/null 2>&1 || init_status=$?
assert_exit_code "$init_status" '1' "init did not stop after the configured restart limit"
assert_file_contains "$WORK_DIR/init_console.log" '^rescue$' "init --setenv did not export environment overrides to the child"

restart_lines=$(grep -c '^rescue$' "$WORK_DIR/init_console.log")
[ "$restart_lines" -eq 3 ] || fail "init did not run the child the expected number of times with --max-restarts"

mkdir -p "$WORK_DIR/init_fakebin"
printf '#!/bin/sh\nprintf hijacked\\n\n' > "$WORK_DIR/init_fakebin/true"
chmod +x "$WORK_DIR/init_fakebin/true"
init_path_status=0
PATH="$WORK_DIR/init_fakebin:$PATH" "$ROOT_DIR/build/init" -n -q true >"$WORK_DIR/init_path.out" 2>"$WORK_DIR/init_path.err" || init_path_status=$?
assert_exit_code "$init_path_status" '1' "init should reject non-absolute program paths"
assert_file_contains "$WORK_DIR/init_path.err" 'refusing non-absolute program path' "init did not explain the path hardening failure"
