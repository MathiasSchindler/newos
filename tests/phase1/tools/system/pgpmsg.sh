#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup pgpmsg

SAMPLE_KEY="$ROOT_DIR/experimental/pgp-keys/86BBADD51B38D4F21FE8C46C99D37C39FA2C23A8.asc"

"${TEST_BIN_DIR}/pgpmsg" --help > "$WORK_DIR/help.out" 2>&1
assert_file_contains "$WORK_DIR/help.out" '^Usage: pgpmsg ' "pgpmsg --help did not print usage"
assert_file_contains "$WORK_DIR/help.out" 'encrypt -r RECIPIENT' "pgpmsg usage did not include the encrypt command shape"

"${TEST_BIN_DIR}/pgpmsg" inspect "$SAMPLE_KEY" > "$WORK_DIR/inspect.out"
assert_file_contains "$WORK_DIR/inspect.out" '^packet 1: tag 6 (public key), length 525$' "pgpmsg inspect did not list the public-key packet"
assert_file_contains "$WORK_DIR/inspect.out" 'tag 2 (signature)' "pgpmsg inspect did not list signature packets"
assert_file_contains "$WORK_DIR/inspect.out" 'tag 14 (public subkey)' "pgpmsg inspect did not list subkey packets"

"${TEST_BIN_DIR}/pgpmsg" --json inspect "$SAMPLE_KEY" > "$WORK_DIR/inspect.jsonl"
assert_file_contains "$WORK_DIR/inspect.jsonl" '"schema":"newos.tool.v1"' "pgpmsg --json inspect did not use the shared JSON envelope"
assert_file_contains "$WORK_DIR/inspect.jsonl" '"event":"packet"' "pgpmsg --json inspect did not emit packet events"
assert_file_contains "$WORK_DIR/inspect.jsonl" '"name":"public key"' "pgpmsg --json inspect did not name public-key packets"

if "${TEST_BIN_DIR}/pgpmsg" verify "$SAMPLE_KEY" > "$WORK_DIR/verify.out" 2> "$WORK_DIR/verify.err"; then
    fail "pgpmsg verify reported cryptographic success before verification is implemented"
fi
assert_file_contains "$WORK_DIR/verify.out" '^signature: not checked$' "pgpmsg verify did not report signature metadata status"
assert_file_contains "$WORK_DIR/verify.out" '^issuer: 99d37c39fa2c23a8$' "pgpmsg verify did not report the signature issuer key ID"
assert_file_contains "$WORK_DIR/verify.err" 'cryptographic verification is not implemented yet' "pgpmsg verify did not explain the nonzero status"

if "${TEST_BIN_DIR}/pgpmsg" encrypt -r 99d37c39fa2c23a8 "$WORK_DIR/plain.txt" > "$WORK_DIR/encrypt.out" 2> "$WORK_DIR/encrypt.err"; then
    fail "pgpmsg encrypt unexpectedly succeeded"
fi
assert_file_contains "$WORK_DIR/encrypt.err" 'OpenPGP encryption is not implemented yet' "pgpmsg encrypt did not report an explicit unsupported operation"

if "${TEST_BIN_DIR}/pgpmsg" decrypt "$WORK_DIR/message.pgp" > "$WORK_DIR/decrypt.out" 2> "$WORK_DIR/decrypt.err"; then
    fail "pgpmsg decrypt unexpectedly succeeded"
fi
assert_file_contains "$WORK_DIR/decrypt.err" 'OpenPGP decryption is not implemented yet' "pgpmsg decrypt did not report an explicit unsupported operation"

if "${TEST_BIN_DIR}/pgpmsg" sign -u 99d37c39fa2c23a8 "$WORK_DIR/plain.txt" > "$WORK_DIR/sign.out" 2> "$WORK_DIR/sign.err"; then
    fail "pgpmsg sign unexpectedly succeeded"
fi
assert_file_contains "$WORK_DIR/sign.err" 'OpenPGP signing is not implemented yet' "pgpmsg sign did not report an explicit unsupported operation"
