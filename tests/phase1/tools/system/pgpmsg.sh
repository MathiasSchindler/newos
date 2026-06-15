#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup pgpmsg

SAMPLE_KEY="$ROOT_DIR/experimental/pgp-keys/86BBADD51B38D4F21FE8C46C99D37C39FA2C23A8.asc"
PGPKEY_BIN="$TEST_BIN_DIR/pgpkey"

printf 'hello pgpmsg\n' > "$WORK_DIR/plain.txt"

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

if [ ! -x "$PGPKEY_BIN" ]; then
    PGPKEY_BIN="$ROOT_DIR/build/host-macos-aarch64/pgpkey"
fi
"$PGPKEY_BIN" generate --userid "Message Signer <signer@example.com>" --out "$WORK_DIR/secret.asc" --public-out "$WORK_DIR/public.asc" --no-passphrase > "$WORK_DIR/keygen.out"
SIGNER_FPR=$(sed -n 's/^generated: //p' "$WORK_DIR/keygen.out" | head -1)
"$PGPKEY_BIN" generate --userid "Message Recipient <recipient@example.com>" --out "$WORK_DIR/recipient-secret.asc" --public-out "$WORK_DIR/recipient-public.asc" --no-passphrase > "$WORK_DIR/recipient-keygen.out"
RECIPIENT_FPR=$(sed -n 's/^generated: //p' "$WORK_DIR/recipient-keygen.out" | head -1)
"${TEST_BIN_DIR}/pgpmsg" sign --detach --armor -s "$WORK_DIR/secret.asc" -u "$SIGNER_FPR" -o "$WORK_DIR/plain.sig.asc" "$WORK_DIR/plain.txt"
assert_file_contains "$WORK_DIR/plain.sig.asc" '^-----BEGIN PGP SIGNATURE-----$' "pgpmsg sign did not write an armored detached signature"
"${TEST_BIN_DIR}/pgpmsg" verify -k "$WORK_DIR/public.asc" "$WORK_DIR/plain.sig.asc" "$WORK_DIR/plain.txt" > "$WORK_DIR/verify_good.out"
assert_file_contains "$WORK_DIR/verify_good.out" '^signature: good$' "pgpmsg verify did not report a good detached signature"
"${TEST_BIN_DIR}/pgpmsg" --json verify -k "$WORK_DIR/public.asc" "$WORK_DIR/plain.sig.asc" "$WORK_DIR/plain.txt" > "$WORK_DIR/verify_good.jsonl"
assert_file_contains "$WORK_DIR/verify_good.jsonl" '"event":"signature"' "pgpmsg --json verify did not emit a signature event"
assert_file_contains "$WORK_DIR/verify_good.jsonl" '"status":"good"' "pgpmsg --json verify did not report a good detached signature"
printf 'tampered\n' > "$WORK_DIR/tampered.txt"
if "${TEST_BIN_DIR}/pgpmsg" verify -k "$WORK_DIR/public.asc" "$WORK_DIR/plain.sig.asc" "$WORK_DIR/tampered.txt" > "$WORK_DIR/verify_bad.out" 2> "$WORK_DIR/verify_bad.err"; then
    fail "pgpmsg verify accepted a tampered detached signature"
fi
assert_file_contains "$WORK_DIR/verify_bad.out" '^signature: bad$' "pgpmsg verify did not report a bad detached signature"
if "${TEST_BIN_DIR}/pgpmsg" --json verify -k "$WORK_DIR/public.asc" "$WORK_DIR/plain.sig.asc" "$WORK_DIR/tampered.txt" > "$WORK_DIR/verify_bad.jsonl" 2> "$WORK_DIR/verify_bad_json.err"; then
    fail "pgpmsg --json verify accepted a tampered detached signature"
fi
assert_file_contains "$WORK_DIR/verify_bad.jsonl" '"status":"bad"' "pgpmsg --json verify did not report a bad detached signature"

"${TEST_BIN_DIR}/pgpmsg" encrypt -k "$WORK_DIR/public.asc" -r "$SIGNER_FPR" --armor -o "$WORK_DIR/plain.pgp.asc" "$WORK_DIR/plain.txt"
assert_file_contains "$WORK_DIR/plain.pgp.asc" '^-----BEGIN PGP MESSAGE-----$' "pgpmsg encrypt did not write an armored message"
"${TEST_BIN_DIR}/pgpmsg" inspect "$WORK_DIR/plain.pgp.asc" > "$WORK_DIR/encrypted.inspect"
assert_file_contains "$WORK_DIR/encrypted.inspect" 'tag 1 (public-key encrypted session key)' "pgpmsg encrypt did not write a public-key encrypted session key packet"
assert_file_contains "$WORK_DIR/encrypted.inspect" 'tag 18 (symmetrically encrypted integrity protected data)' "pgpmsg encrypt did not write an integrity-protected encrypted data packet"
"${TEST_BIN_DIR}/pgpmsg" decrypt -s "$WORK_DIR/secret.asc" -o "$WORK_DIR/plain.dec" "$WORK_DIR/plain.pgp.asc"
cmp "$WORK_DIR/plain.txt" "$WORK_DIR/plain.dec" || fail "pgpmsg decrypt did not recover the original plaintext"

"$PGPKEY_BIN" -k "$WORK_DIR/pubring.pgp" import "$WORK_DIR/public.asc" "$WORK_DIR/recipient-public.asc" > "$WORK_DIR/pubring-import.out"
"${TEST_BIN_DIR}/pgpmsg" encrypt -k "$WORK_DIR/pubring.pgp" -r "$SIGNER_FPR" -r "$RECIPIENT_FPR" --armor -o "$WORK_DIR/plain-multi.pgp.asc" "$WORK_DIR/plain.txt"
"${TEST_BIN_DIR}/pgpmsg" inspect "$WORK_DIR/plain-multi.pgp.asc" > "$WORK_DIR/encrypted-multi.inspect"
PKESK_COUNT=$(grep -c 'tag 1 (public-key encrypted session key)' "$WORK_DIR/encrypted-multi.inspect")
if [ "$PKESK_COUNT" -ne 2 ]; then
    fail "pgpmsg encrypt did not write one session-key packet per recipient"
fi
"${TEST_BIN_DIR}/pgpmsg" decrypt -s "$WORK_DIR/secret.asc" -o "$WORK_DIR/plain-multi-signer.dec" "$WORK_DIR/plain-multi.pgp.asc"
cmp "$WORK_DIR/plain.txt" "$WORK_DIR/plain-multi-signer.dec" || fail "pgpmsg decrypt did not recover a multi-recipient message with the first secret key"
"${TEST_BIN_DIR}/pgpmsg" decrypt -s "$WORK_DIR/recipient-secret.asc" -o "$WORK_DIR/plain-multi-recipient.dec" "$WORK_DIR/plain-multi.pgp.asc"
cmp "$WORK_DIR/plain.txt" "$WORK_DIR/plain-multi-recipient.dec" || fail "pgpmsg decrypt did not recover a multi-recipient message with the second secret key"
