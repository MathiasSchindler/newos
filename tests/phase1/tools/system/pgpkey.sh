#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup pgpkey

SAMPLE_KEY="$ROOT_DIR/experimental/pgp-keys/86BBADD51B38D4F21FE8C46C99D37C39FA2C23A8.asc"
SPIEGEL_KEY="$ROOT_DIR/experimental/pgp-keys/SPIEGEL_Verlag_Hamburg_27FF8ADC_Public.asc.txt"
RWIEGAND_KEY="$ROOT_DIR/experimental/pgp-keys/rwiegand.asc"
KEYRING="$WORK_DIR/pubring.pgp"
SQL_BIN="$TEST_BIN_DIR/sql"

if [ ! -x "$SQL_BIN" ]; then
    SQL_BIN="$ROOT_DIR/build/host-macos-aarch64/sql"
fi

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
assert_file_contains "$WORK_DIR/show_color.out" "${ESC}\\[1;32mnot expired" "pgpkey show --color=always did not color a non-expired expiration status"
assert_file_contains "$WORK_DIR/show_color.out" "${ESC}\\[1;31mSHA-1" "pgpkey show --color=always did not color SHA-1 as weak"
assert_file_contains "$WORK_DIR/show_color.out" "${ESC}\\[1;33mTripleDES" "pgpkey show --color=always did not color TripleDES as legacy"
"${TEST_BIN_DIR}/pgpkey" show "$SPIEGEL_KEY" > "$WORK_DIR/spiegel_show.out"
assert_file_contains "$WORK_DIR/spiegel_show.out" '^key-expires: 2025-05-11 (expired)$' "pgpkey show did not mark the expired SPIEGEL primary key"
assert_file_contains "$WORK_DIR/spiegel_show.out" '^subkey-expires: 2025-05-11 (expired)$' "pgpkey show did not mark the expired SPIEGEL subkey"
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

"${TEST_BIN_DIR}/pgpkey" issuers "$SAMPLE_KEY" > "$WORK_DIR/issuers.out"
assert_file_contains "$WORK_DIR/issuers.out" '^99d37c39fa2c23a8 86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8$' "pgpkey issuers did not report self-issued signatures with issuer fingerprint"
"${TEST_BIN_DIR}/pgpkey" issuers --external "$SAMPLE_KEY" "$SPIEGEL_KEY" "$RWIEGAND_KEY" > "$WORK_DIR/external_issuers.out"
assert_file_contains "$WORK_DIR/external_issuers.out" '^1d9a6fc1222685f3$' "pgpkey issuers --external did not report an external issuer key ID"
assert_file_contains "$WORK_DIR/external_issuers.out" '^0e2864d1bc71fa99$' "pgpkey issuers --external did not report the missing external issuer key ID"
if grep '^99d37c39fa2c23a8' "$WORK_DIR/external_issuers.out" >/dev/null 2>&1; then
    fail "pgpkey issuers --external included a key already present in the input"
fi
"${TEST_BIN_DIR}/pgpkey" --json issuers --external "$SPIEGEL_KEY" > "$WORK_DIR/issuers.jsonl"
assert_file_contains "$WORK_DIR/issuers.jsonl" '"event":"issuer"' "pgpkey --json issuers did not emit issuer events"
assert_file_contains "$WORK_DIR/issuers.jsonl" '"key_id":"1d9a6fc1222685f3"' "pgpkey --json issuers did not report issuer key IDs"

"${TEST_BIN_DIR}/pgpkey" catalog-sql "$SAMPLE_KEY" "$SPIEGEL_KEY" > "$WORK_DIR/catalog.sql"
assert_file_contains "$WORK_DIR/catalog.sql" '^CREATE TABLE IF NOT EXISTS certs' "pgpkey catalog-sql did not emit the certs schema"
assert_file_contains "$WORK_DIR/catalog.sql" 'dcb4218362db86f3' "pgpkey catalog-sql did not emit third-party issuer metadata"
"$SQL_BIN" "$WORK_DIR/pgpkeys.sqs" < "$WORK_DIR/catalog.sql" > "$WORK_DIR/catalog-build.out"
"$SQL_BIN" "$WORK_DIR/pgpkeys.sqs" "SELECT key_id, primary_uid FROM certs WHERE key_id = 'ad6975a127ff8adc';" > "$WORK_DIR/catalog-certs.out"
assert_file_contains "$WORK_DIR/catalog-certs.out" 'ad6975a127ff8adc.*SPIEGEL-Verlag Hamburg' "SQL catalog did not store certificate primary UID metadata"
"$SQL_BIN" "$WORK_DIR/pgpkeys.sqs" "SELECT issuer_key_id, issuer_fingerprint FROM signatures WHERE issuer_key_id = 'dcb4218362db86f3';" > "$WORK_DIR/catalog-issuers.out"
assert_file_contains "$WORK_DIR/catalog-issuers.out" 'dcb4218362db86f3.*77278938cb824d15e2f6c855dcb4218362db86f3' "SQL catalog did not store signature issuer fingerprint metadata"
"${TEST_BIN_DIR}/pgpkey" show -v "$SPIEGEL_KEY" --keystore "$WORK_DIR/pgpkeys.sqs" > "$WORK_DIR/show_keystore.out"
assert_file_contains "$WORK_DIR/show_keystore.out" 'issuer-uid SPIEGEL-Verlag Hamburg <Investigativ@spiegel.de>' "pgpkey show --keystore did not resolve local issuer UID labels"
STORE_DIR="$WORK_DIR/store"
"${TEST_BIN_DIR}/pgpkey" store init "$STORE_DIR" > "$WORK_DIR/store-init.out"
"${TEST_BIN_DIR}/pgpkey" store import "$STORE_DIR" "$SAMPLE_KEY" "$SPIEGEL_KEY" > "$WORK_DIR/store-import.out"
"${TEST_BIN_DIR}/pgpkey" --store "$STORE_DIR" list > "$WORK_DIR/store-list.out"
assert_file_contains "$WORK_DIR/store-list.out" 'SPIEGEL-Verlag Hamburg' "pgpkey --store list did not read the store keyring"
"${TEST_BIN_DIR}/pgpkey" show -v "$SPIEGEL_KEY" --store "$STORE_DIR" > "$WORK_DIR/show_store.out"
assert_file_contains "$WORK_DIR/show_store.out" 'issuer-uid SPIEGEL-Verlag Hamburg <Investigativ@spiegel.de>' "pgpkey show --store did not resolve store issuer UID labels"
"${TEST_BIN_DIR}/pgpkey" store rebuild-index "$STORE_DIR" > "$WORK_DIR/store-reindex.out"
assert_file_contains "$WORK_DIR/store-reindex.out" 'indexed: .*pgpkeys\.sqs' "pgpkey store rebuild-index did not report the rebuilt index"

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
assert_file_contains "$WORK_DIR/generate.out" '^profile: rfc9580$' "pgpkey generate did not report the default RFC 9580 profile"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^primary: public primary, v6, Ed25519, 256 bits, created ' "pgpkey show did not summarize the generated RFC 9580 Ed25519 public key"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^uid: Test User <test@example.com>$' "pgpkey show did not print the generated user ID"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^primary-uid: Test User <test@example.com>$' "pgpkey show did not mark the generated primary user ID"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^key-flags: certify, sign$' "pgpkey show did not decode generated key flags"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^key-expires: ' "pgpkey show did not decode generated expiration metadata"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^preferred-symmetric: AES-256, AES-128$' "pgpkey show did not decode generated RFC 9580 cipher preferences"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^preferred-hash: SHA-512, SHA-256$' "pgpkey show did not decode generated RFC 9580 hash preferences"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^preferred-compression: uncompressed$' "pgpkey show did not decode generated RFC 9580 compression preferences"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^subkey: public subkey, v6, X25519, 256 bits, created ' "pgpkey generate did not add an RFC 9580 X25519 encryption subkey"
assert_file_contains "$WORK_DIR/generated_public_show.out" '^subkey-flags: encrypt communications, encrypt storage$' "pgpkey show did not decode generated encryption subkey flags"

"${TEST_BIN_DIR}/pgpkey" show "$WORK_DIR/secret.asc" > "$WORK_DIR/generated_secret_show.out"
assert_file_contains "$WORK_DIR/generated_secret_show.out" '^primary: secret primary, v6, Ed25519, 256 bits, created ' "pgpkey show did not summarize the generated RFC 9580 Ed25519 private key"
assert_file_contains "$WORK_DIR/generated_secret_show.out" '^subkey: secret subkey, v6, X25519, 256 bits, created ' "pgpkey show did not summarize the generated RFC 9580 X25519 private subkey"
PUBLIC_FPR=$(sed -n 's/^fingerprint: //p' "$WORK_DIR/generated_public_show.out" | head -1)
SECRET_FPR=$(sed -n 's/^fingerprint: //p' "$WORK_DIR/generated_secret_show.out" | head -1)
if [ -z "$PUBLIC_FPR" ] || [ "$PUBLIC_FPR" != "$SECRET_FPR" ]; then
    fail "pgpkey generated public and private fingerprints differ"
fi

"${TEST_BIN_DIR}/pgpkey" generate --legacy-v4 --userid "Legacy User <legacy@example.com>" --out "$WORK_DIR/legacy-secret.asc" --public-out "$WORK_DIR/legacy-public.asc" --no-passphrase > "$WORK_DIR/legacy-generate.out"
assert_file_contains "$WORK_DIR/legacy-generate.out" '^profile: legacy-v4$' "pgpkey generate --legacy-v4 did not report the legacy profile"
assert_file_contains "$WORK_DIR/legacy-generate.out" '^warning: legacy v4 key material is deprecated by RFC 9580$' "pgpkey generate --legacy-v4 did not warn about legacy output"
"${TEST_BIN_DIR}/pgpkey" show "$WORK_DIR/legacy-public.asc" > "$WORK_DIR/legacy-public-show.out"
assert_file_contains "$WORK_DIR/legacy-public-show.out" '^primary: public primary, v4, EdDSA legacy, 256 bits, created ' "pgpkey generate --legacy-v4 did not create a v4 EdDSA legacy primary key"
assert_file_contains "$WORK_DIR/legacy-public-show.out" '^subkey: public subkey, v4, ECDH, 256 bits, created ' "pgpkey generate --legacy-v4 did not create a v4 ECDH subkey"

"${TEST_BIN_DIR}/pgpkey" edit "$WORK_DIR/legacy-secret.asc" --out "$WORK_DIR/edit-add-secret.asc" --public-out "$WORK_DIR/edit-add-public.asc" --add-uid "Added User <added@example.com>" > "$WORK_DIR/edit-add.out"
assert_file_contains "$WORK_DIR/edit-add.out" '^edited$' "pgpkey edit --add-uid did not report success"
"${TEST_BIN_DIR}/pgpkey" show "$WORK_DIR/edit-add-public.asc" > "$WORK_DIR/edit-add-show.out"
assert_file_contains "$WORK_DIR/edit-add-show.out" '^uid: Added User <added@example.com>$' "pgpkey edit --add-uid did not add a user ID"
"${TEST_BIN_DIR}/pgpkey" edit "$WORK_DIR/edit-add-secret.asc" --out "$WORK_DIR/edit-primary-secret.asc" --public-out "$WORK_DIR/edit-primary-public.asc" --set-primary-uid "Added User <added@example.com>" > "$WORK_DIR/edit-primary.out"
"${TEST_BIN_DIR}/pgpkey" show "$WORK_DIR/edit-primary-public.asc" > "$WORK_DIR/edit-primary-show.out"
assert_file_contains "$WORK_DIR/edit-primary-show.out" '^primary-uid: Added User <added@example.com>$' "pgpkey edit --set-primary-uid did not update the primary UID"
"${TEST_BIN_DIR}/pgpkey" edit "$WORK_DIR/edit-primary-secret.asc" --out "$WORK_DIR/edit-subkey-secret.asc" --public-out "$WORK_DIR/edit-subkey-public.asc" --add-subkey > "$WORK_DIR/edit-subkey.out"
"${TEST_BIN_DIR}/pgpkey" show "$WORK_DIR/edit-subkey-public.asc" > "$WORK_DIR/edit-subkey-show.out"
SUBKEY_COUNT=$(grep -c '^subkey-fingerprint: ' "$WORK_DIR/edit-subkey-show.out")
if [ "$SUBKEY_COUNT" -lt 2 ]; then
    fail "pgpkey edit --add-subkey did not add a second subkey"
fi
"${TEST_BIN_DIR}/pgpkey" edit "$WORK_DIR/edit-primary-secret.asc" --out "$WORK_DIR/edit-revoke-secret.asc" --public-out "$WORK_DIR/edit-revoke-public.asc" --revoke-uid "Added User <added@example.com>" > "$WORK_DIR/edit-revoke.out"
"${TEST_BIN_DIR}/pgpkey" show -v "$WORK_DIR/edit-revoke-public.asc" > "$WORK_DIR/edit-revoke-show.out"
assert_file_contains "$WORK_DIR/edit-revoke-show.out" 'certification revocation' "pgpkey edit --revoke-uid did not add a certification revocation signature"
if "${TEST_BIN_DIR}/pgpkey" -k "$KEYRING" import "$WORK_DIR/secret.asc" > "$WORK_DIR/import_secret.out" 2> "$WORK_DIR/import_secret.err"; then
    fail "pgpkey import accepted a private key block"
fi
assert_file_contains "$WORK_DIR/import_secret.err" 'refusing to import private key into public keyring' "pgpkey import did not reject private key material"

"${TEST_BIN_DIR}/pgpkey" --json generate --userid "Json User <json@example.com>" --out "$WORK_DIR/json-secret.asc" --public-out "$WORK_DIR/json-public.asc" --no-passphrase > "$WORK_DIR/generate.jsonl"
assert_file_contains "$WORK_DIR/generate.jsonl" '"event":"generate"' "pgpkey --json generate did not emit a generate event"
assert_file_contains "$WORK_DIR/generate.jsonl" '"profile":"rfc9580"' "pgpkey --json generate did not report the default RFC 9580 profile"
assert_file_contains "$WORK_DIR/generate.jsonl" '"version":6' "pgpkey --json generate did not report version 6"
assert_file_contains "$WORK_DIR/generate.jsonl" '"curve":"Ed25519"' "pgpkey --json generate did not report the curve"
assert_file_contains "$WORK_DIR/generate.jsonl" '"protected":false' "pgpkey --json generate did not report protection status"

"${TEST_BIN_DIR}/pgpkey" -k "$KEYRING" import "$SAMPLE_KEY" > "$WORK_DIR/import.out"
assert_file_contains "$WORK_DIR/import.out" '^imported: 86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8$' "pgpkey import did not report the imported fingerprint"
"${TEST_BIN_DIR}/pgpkey" -k "$KEYRING" import "$SAMPLE_KEY" > "$WORK_DIR/import_again.out"
assert_file_contains "$WORK_DIR/import_again.out" '^unchanged: 86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8$' "pgpkey import did not recognize a duplicate key"
"${TEST_BIN_DIR}/pgpkey" -k "$WORK_DIR/new-only.pgp" import "$WORK_DIR/public.asc" > "$WORK_DIR/import_new_only.out"
cat "$WORK_DIR/new-only.pgp" "$KEYRING" > "$WORK_DIR/import-multi.pgp"
"${TEST_BIN_DIR}/pgpkey" -k "$KEYRING" import "$WORK_DIR/import-multi.pgp" > "$WORK_DIR/import_multi.out"
assert_file_contains "$WORK_DIR/import_multi.out" "^imported: $PUBLIC_FPR$" "pgpkey import did not import the new certificate from a multi-cert file"
assert_file_contains "$WORK_DIR/import_multi.out" '^unchanged: 86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8$' "pgpkey import did not skip the duplicate certificate in a multi-cert file"

"${TEST_BIN_DIR}/pgpkey" -k "$KEYRING" list > "$WORK_DIR/list.out"
assert_file_contains "$WORK_DIR/list.out" '^fingerprint: 86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8$' "pgpkey list did not read the imported keyring"
SAMPLE_IMPORT_COUNT=$(grep -c '^fingerprint: 86bbadd51b38d4f21fe8c46c99d37c39fa2c23a8$' "$WORK_DIR/list.out")
if [ "$SAMPLE_IMPORT_COUNT" -ne 1 ]; then
    fail "pgpkey import duplicated a certificate from a multi-cert file"
fi
assert_file_contains "$WORK_DIR/list.out" "^fingerprint: $PUBLIC_FPR$" "pgpkey list did not include the multi-cert import"

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