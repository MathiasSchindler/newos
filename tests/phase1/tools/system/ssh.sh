#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup ssh

assert_command_succeeds "$ROOT_DIR/build/ssh" --help > "$WORK_DIR/ssh_help.out"
assert_file_contains "$WORK_DIR/ssh_help.out" '^ssh - minimal interactive SSH client$' "ssh --help did not print the tool summary"
assert_file_contains "$WORK_DIR/ssh_help.out" '^Usage: ssh ' "ssh --help did not print usage"

ssh_status=0
"$ROOT_DIR/build/ssh" -p 0 user@example.invalid > "$WORK_DIR/ssh_invalid.out" 2>&1 || ssh_status=$?
assert_exit_code "$ssh_status" '1' "ssh should reject an invalid port number"
assert_file_contains "$WORK_DIR/ssh_invalid.out" '^Usage: ssh ' "ssh did not print usage for an invalid port"
