#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup ssh

assert_command_succeeds "${TEST_BIN_DIR}/ssh" --help > "$WORK_DIR/ssh_help.out"
assert_file_contains "$WORK_DIR/ssh_help.out" '^ssh - minimal interactive SSH client$' "ssh --help did not print the tool summary"
assert_file_contains "$WORK_DIR/ssh_help.out" '^Usage: ssh ' "ssh --help did not print usage"

ssh_status=0
"${TEST_BIN_DIR}/ssh" -p 0 user@example.invalid > "$WORK_DIR/ssh_invalid.out" 2>&1 || ssh_status=$?
assert_exit_code "$ssh_status" '1' "ssh should reject an invalid port number"
assert_file_contains "$WORK_DIR/ssh_invalid.out" '^Usage: ssh ' "ssh did not print usage for an invalid port"

ssh_host_status=0
"${TEST_BIN_DIR}/ssh" 'user@bad host' > "$WORK_DIR/ssh_bad_host.out" 2>&1 || ssh_host_status=$?
assert_exit_code "$ssh_host_status" '1' "ssh should reject destinations with unsafe whitespace"
assert_file_contains "$WORK_DIR/ssh_bad_host.out" 'invalid destination' "ssh did not reject an unsafe destination string"

ssh_user_status=0
"${TEST_BIN_DIR}/ssh" -l 'bad user' example.invalid > "$WORK_DIR/ssh_bad_user.out" 2>&1 || ssh_user_status=$?
assert_exit_code "$ssh_user_status" '1' "ssh should reject unsafe remote user names"
assert_file_contains "$WORK_DIR/ssh_bad_user.out" 'invalid remote user' "ssh did not reject an unsafe remote user name"

printf 'scp-local\n' > "$WORK_DIR/scp_source.txt"
assert_command_succeeds "${TEST_BIN_DIR}/scp" "$WORK_DIR/scp_source.txt" "$WORK_DIR/scp_copy.txt"
assert_file_contains "$WORK_DIR/scp_copy.txt" '^scp-local$' "scp did not perform a local-to-local copy"

scp_remote_status=0
"${TEST_BIN_DIR}/scp" "$WORK_DIR/scp_source.txt" user@example.invalid:/tmp/scp_source.txt > "$WORK_DIR/scp_remote.out" 2>&1 || scp_remote_status=$?
assert_exit_code "$scp_remote_status" '1' "scp should reject remote operands until remote transfer is implemented"
assert_file_contains "$WORK_DIR/scp_remote.out" 'remote transfers are not available yet' "scp did not explain the remote-transfer limitation"
