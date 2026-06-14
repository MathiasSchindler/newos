#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup pgpkey

SAMPLE_KEY="$ROOT_DIR/experimental/pgp-keys/86BBADD51B38D4F21FE8C46C99D37C39FA2C23A8.asc"
KEYRING="$WORK_DIR/pubring.pgp"

"${TEST_BIN_DIR}/pgpkey" show "$SAMPLE_KEY" > "$WORK_DIR/show.out"
assert_file_contains "$WORK_DIR/show.out" '^primary: public primary, v4, RSA encrypt/sign, 4096 bits, created 2016-03-01$' "pgpkey show did not summarize the RSA primary key"
assert_file_contains "$WORK_DIR/show.out" '^fingerprint: 86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8$' "pgpkey show did not print the expected fingerprint"
assert_file_contains "$WORK_DIR/show.out" '^key-id: 99d37c39fa2c23a8$' "pgpkey show did not print the expected key id"
assert_file_contains "$WORK_DIR/show.out" 'GFF e.V.' "pgpkey show did not print user IDs"
assert_file_contains "$WORK_DIR/show.out" '^primary-uid: GFF e.V.' "pgpkey show did not print the primary UID marker"
assert_file_contains "$WORK_DIR/show.out" '^key-flags: certify, sign$' "pgpkey show did not print primary-key flags"
assert_file_contains "$WORK_DIR/show.out" '^key-expires: 2027-04-13 ' "pgpkey show did not print the key expiration date"
assert_file_contains "$WORK_DIR/show.out" '^preferred-symmetric: AES-256, AES-192, AES-128, CAST5, TripleDES, IDEA$' "pgpkey show did not print cipher preferences"
assert_file_contains "$WORK_DIR/show.out" '^preferred-hash: SHA-256, SHA-1, SHA-384, SHA-512, SHA-224$' "pgpkey show did not print hash preferences"
assert_file_contains "$WORK_DIR/show.out" '^preferred-compression: ZLIB, BZip2, ZIP$' "pgpkey show did not print compression preferences"
assert_file_contains "$WORK_DIR/show.out" '^subkey-flags: encrypt communications, encrypt storage$' "pgpkey show did not print subkey flags"
assert_file_contains "$WORK_DIR/show.out" '^subkey-expires: 2027-04-13 ' "pgpkey show did not print subkey expiration"
assert_file_contains "$WORK_DIR/show.out" '^subkey: public subkey, v4, RSA encrypt/sign, 4096 bits, created 2016-03-01$' "pgpkey show did not summarize the RSA subkey"

ESC=$(printf '\033')
"${TEST_BIN_DIR}/pgpkey" show --color=always "$SAMPLE_KEY" > "$WORK_DIR/show_color.out"
assert_file_contains "$WORK_DIR/show_color.out" "${ESC}\\[1;32mvalid" "pgpkey show --color=always did not color a valid expiration status"
assert_file_contains "$WORK_DIR/show_color.out" "${ESC}\\[1;31mSHA-1" "pgpkey show --color=always did not color SHA-1 as weak"
assert_file_contains "$WORK_DIR/show_color.out" "${ESC}\\[1;33mTripleDES" "pgpkey show --color=always did not color TripleDES as legacy"
"${TEST_BIN_DIR}/pgpkey" --no-color show "$SAMPLE_KEY" > "$WORK_DIR/show_no_color.out"
if grep "${ESC}\\[" "$WORK_DIR/show_no_color.out" >/dev/null 2>&1; then
    fail "pgpkey --no-color emitted ANSI color escapes"
fi

"${TEST_BIN_DIR}/pgpkey" show -v "$SAMPLE_KEY" > "$WORK_DIR/show_verbose.out"
assert_file_contains "$WORK_DIR/show_verbose.out" '^signatures:$' "pgpkey show -v did not print signature details"
assert_file_contains "$WORK_DIR/show_verbose.out" '^signature 3: positive user ID certification, v4, RSA encrypt/sign, SHA-256, created 2026-04-13, uid 1, issuer 99d37c39fa2c23a8, issuer-fpr 86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8, key-expires 2027-04-13 .* after 350778858 seconds, flags certify, sign, primary-uid yes$' "pgpkey show -v did not decode self-signature metadata"
assert_file_contains "$WORK_DIR/show_verbose.out" '^signature 46: subkey binding, v4, RSA encrypt/sign, SHA-256, created 2026-04-13, subkey 1, issuer 99d37c39fa2c23a8, issuer-fpr 86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8, key-expires 2027-04-13 .* after 350778865 seconds, flags encrypt communications, encrypt storage$' "pgpkey show -v did not decode subkey binding metadata"

"${TEST_BIN_DIR}/pgpkey" packets "$SAMPLE_KEY" > "$WORK_DIR/packets.out"
assert_file_contains "$WORK_DIR/packets.out" '^packet 1: tag 6 (public key), length 525$' "pgpkey packets did not list the public-key packet"
assert_file_contains "$WORK_DIR/packets.out" 'tag 14 (public subkey)' "pgpkey packets did not list the public subkey"

if "${TEST_BIN_DIR}/pgpkey" generate --userid "Test User <test@example.com>" --out "$WORK_DIR/secret.asc" --public-out "$WORK_DIR/public.asc" > "$WORK_DIR/generate_no_ack.out" 2> "$WORK_DIR/generate_no_ack.err"; then
    fail "pgpkey generate created an unprotected secret key without --no-passphrase"
fi
assert_file_contains "$WORK_DIR/generate_no_ack.err" 'passphrase-protected secret keys are not implemented yet' "pgpkey generate did not require --no-passphrase"

"${TEST_BIN_DIR}/pgpkey" generate --userid "Test User <test@example.com>" --out "$WORK_DIR/secret.asc" --public-out "$WORK_DIR/public.asc" --no-passphrase --expires 2y > "$WORK_DIR/generate.out"
assert_file_contains "$WORK_DIR/generate.out" '^generated: [0-9a-f][0-9a-f]' "pgpkey generate did not report a fingerprint"
assert_file_contains "$WORK_DIR/generate.out" '^warning: secret key is not passphrase-protected$' "pgpkey generate did not warn about unprotected output"
assert_file_contains "$WORK_DIR/secret.asc" '^-----BEGIN PGP PRIVATE KEY BLOCK-----$' "pgpkey generate did not write a private key block"
assert_file_contains "$WORK_DIR/public.asc" '^-----BEGIN PGP PUBLIC KEY BLOCK-----$' "pgpkey generate did not write a public key block"

"${TEST_BIN_DIR}/pgpkey" show "$WORK_DIR/public.asc" > "$WORK_DIR/generated_public_show.out"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^primary: public primary, v4, EdDSA, 256 bits, created ' "pgpkey show did not summarize the generated Ed25519 public key"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^uid: Test User <test@example.com>$' "pgpkey show did not print the generated user ID"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^primary-uid: Test User <test@example.com>$' "pgpkey show did not mark the generated primary user ID"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^key-flags: certify, sign$' "pgpkey show did not decode generated key flags"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^key-expires: ' "pgpkey show did not decode generated expiration metadata"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^subkey: public subkey, v4, ECDH, 256 bits, created ' "pgpkey generate did not add an X25519 encryption subkey"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^subkey-flags: encrypt communications, encrypt storage$' "pgpkey show did not decode generated encryption subkey flags"

"${TEST_BIN_DIR}/pgpkey" show "$WORK_DIR/secret.asc" > "$WORK_DIR/generated_secret_show.out"
assert_file_contains "$WORK_DIR/generated_secret_show.out" '^primary: secret primary, v4, EdDSA, 256 bits, created ' "pgpkey show did not summarize the generated Ed25519 private key"
assert_file_contains "$WORK_DIR/generated_secret_show.out" '^subkey: secret subkey, v4, ECDH, 256 bits, created ' "pgpkey show did not summarize the generated X25519 private subkey"
PUBLIC_FPR=$(sed -n 's/^fingerprint: //p' "$WORK_DIR/generated_public_show.out" | head -1)
SECRET_FPR=$(sed -n 's/^fingerprint: //p' "$WORK_DIR/generated_secret_show.out" | head -1)
if [ -z "$PUBLIC_FPR" ] || [ "$PUBLIC_FPR" != "$SECRET_FPR" ]; then
    fail "pgpkey generated public and private fingerprints differ"
fi
if "${TEST_BIN_DIR}/pgpkey" -k "$KEYRING" import "$WORK_DIR/secret.asc" > "$WORK_DIR/import_secret.out" 2> "$WORK_DIR/import_secret.err"; then
    fail "pgpkey import accepted a private key block"
fi
assert_file_contains "$WORK_DIR/import_secret.err" 'refusing to import private key into public keyring' "pgpkey import did not reject private key material"

"${TEST_BIN_DIR}/pgpkey" --json generate --userid "Json User <json@example.com>" --out "$WORK_DIR/json-secret.asc" --public-out "$WORK_DIR/json-public.asc" --no-passphrase > "$WORK_DIR/generate.jsonl"
assert_file_contains "$WORK_DIR/generate.jsonl" '"event":"generate"' "pgpkey --json generate did not emit a generate event"
assert_file_contains "$WORK_DIR/generate.jsonl" '"curve":"Ed25519"' "pgpkey --json generate did not report the curve"
assert_file_contains "$WORK_DIR/generate.jsonl" '"protected":false' "pgpkey --json generate did not report protection status"

"${TEST_BIN_DIR}/pgpkey" -k "$KEYRING" import "$SAMPLE_KEY" > "$WORK_DIR/import.out"
assert_file_contains "$WORK_DIR/import.out" '^imported: 86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8$' "pgpkey import did not report the imported fingerprint"
"${TEST_BIN_DIR}/pgpkey" -k "$KEYRING" import "$SAMPLE_KEY" > "$WORK_DIR/import_again.out"
assert_file_contains "$WORK_DIR/import_again.out" '^unchanged: 86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8$' "pgpkey import did not recognize a duplicate key"

"${TEST_BIN_DIR}/pgpkey" -k "$KEYRING" list > "$WORK_DIR/list.out"
assert_file_contains "$WORK_DIR/list.out" '^fingerprint: 86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8$' "pgpkey list did not read the imported keyring"

"${TEST_BIN_DIR}/pgpkey" -k "$KEYRING" --json list > "$WORK_DIR/list.jsonl"
assert_file_contains "$WORK_DIR/list.jsonl" '"schema":"newos.tool.v1"' "pgpkey --json list did not use the shared JSON envelope"
assert_file_contains "$WORK_DIR/list.jsonl" '"event":"certificate"' "pgpkey --json list did not emit certificate events"
assert_file_contains "$WORK_DIR/list.jsonl" '"fingerprint":"86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8"' "pgpkey --json list did not report the fingerprint"
assert_file_contains "$WORK_DIR/list.jsonl" '"signature_infos":48' "pgpkey --json list did not report decoded signature metadata count"

"${TEST_BIN_DIR}/pgpkey" -k "$KEYRING" export 99d37c39fa2c23a8 "$WORK_DIR/export.asc"
assert_file_contains "$WORK_DIR/export.asc" '^-----BEGIN PGP PUBLIC KEY BLOCK-----$' "pgpkey export did not write an armored public key"
"${TEST_BIN_DIR}/pgpkey" show "$WORK_DIR/export.asc" > "$WORK_DIR/export_show.out"
assert_file_contains "$WORK_DIR/export_show.out" '^fingerprint: 86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8$' "pgpkey export did not round-trip to the same fingerprint"

if "${TEST_BIN_DIR}/pgpkey" -k "$KEYRING" export deadbeef > "$WORK_DIR/missing.out" 2> "$WORK_DIR/missing.err"; then
    fail "pgpkey export accepted a missing key selector"
fi
assert_file_contains "$WORK_DIR/missing.err" 'key not found' "pgpkey export did not report a missing key"