#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup getty

"$ROOT_DIR/build/getty" --help > "$WORK_DIR/getty_help.out"
assert_file_contains "$WORK_DIR/getty_help.out" '^Usage: .*getty' "getty --help did not print usage"

assert_command_succeeds "$ROOT_DIR/build/getty" -n -q -i /dev/null "$ROOT_DIR/build/true"

getty_status=0
"$ROOT_DIR/build/getty" -n -q -i /dev/null "$ROOT_DIR/build/sh" -c 'exit 9' >/dev/null 2>&1 || getty_status=$?
assert_exit_code "$getty_status" '9' "getty did not propagate the child exit status"

if "$ROOT_DIR/build/getty" -n -q >/dev/null 2>&1; then
    fail "getty without a tty path should fail"
fi

term_output=$(printf 'console-user\n' | "$ROOT_DIR/build/getty" -n -q -i -t vt100 -p 'login: ' -c 'printf "%s:%s" "$TERM" "$GETTY_USER"' - | tr -d '\r')
case "$term_output" in
    *'login: '*) : ;;
    *) fail "getty did not display the configured prompt" ;;
esac
case "$term_output" in
    *'vt100:console-user'*) : ;;
    *) fail "getty did not honor --term/--prompt/--command" ;;
esac

printf 'Custom issue banner\n' > "$WORK_DIR/issue.txt"
: > "$WORK_DIR/getty_tty.log"
assert_command_succeeds "$ROOT_DIR/build/getty" -n -q --issue-file "$WORK_DIR/issue.txt" "$WORK_DIR/getty_tty.log" "$ROOT_DIR/build/true"
assert_file_contains "$WORK_DIR/getty_tty.log" 'Custom issue banner' "getty did not write the custom issue file to the tty path"

mkdir -p "$WORK_DIR/getty_fakebin"
printf '#!/bin/sh\nprintf hijacked\\n\n' > "$WORK_DIR/getty_fakebin/true"
chmod +x "$WORK_DIR/getty_fakebin/true"
getty_path_status=0
PATH="$WORK_DIR/getty_fakebin:$PATH" "$ROOT_DIR/build/getty" -n -q -i /dev/null true >"$WORK_DIR/getty_path.out" 2>"$WORK_DIR/getty_path.err" || getty_path_status=$?
assert_exit_code "$getty_path_status" '1' "getty should reject non-absolute program paths"
assert_file_contains "$WORK_DIR/getty_path.err" 'refusing non-absolute program path' "getty did not explain the path hardening failure"
