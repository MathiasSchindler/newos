#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup pgpmsg

SAMPLE_KEY="$ROOT_DIR/experimental/pgp-keys/86BBADD51B38D4F21FE8C46C99D37C39FA2C23A8.asc"
RAIMOND_KEY="$ROOT_DIR/experimental/pgp-keys/raimond.asc"
ENCRYPTED_MESSAGE="$ROOT_DIR/experimental/pgp-keys/encryptedmessage.txt"
PGPKEY_BIN="$TEST_BIN_DIR/pgpkey"

external_pgp_fixtures_available=1
for required_pgp_fixture in "$SAMPLE_KEY" "$RAIMOND_KEY" "$ENCRYPTED_MESSAGE"; do
    if [ ! -r "$required_pgp_fixture" ]; then
        note "missing optional PGP fixture: $required_pgp_fixture"
        external_pgp_fixtures_available=0
    fi
done

tamper_last_byte() {
    src="$1"
    dst="$2"
    cp "$src" "$dst"
    size=$(wc -c < "$dst" | tr -d ' ')
    if [ "$size" -le 0 ]; then
        fail "cannot tamper empty file: $src"
    fi
    offset=$((size - 1))
    byte_hex=$(dd if="$dst" bs=1 skip="$offset" count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$byte_hex" = 00 ]; then
        replacement=001
    else
        replacement=000
    fi
    printf "\\$replacement" | dd of="$dst" bs=1 seek="$offset" count=1 conv=notrunc 2>/dev/null
}

printf 'hello pgpmsg\n' > "$WORK_DIR/plain.txt"

"${TEST_BIN_DIR}/pgpmsg" --help > "$WORK_DIR/help.out" 2>&1
assert_file_contains "$WORK_DIR/help.out" '^Usage: pgpmsg ' "pgpmsg --help did not print usage"
assert_file_contains "$WORK_DIR/help.out" 'encrypt -r RECIPIENT' "pgpmsg usage did not include the encrypt command shape"

if [ "$external_pgp_fixtures_available" -eq 1 ]; then
"${TEST_BIN_DIR}/pgpmsg" inspect "$SAMPLE_KEY" > "$WORK_DIR/inspect.out"
assert_file_contains "$WORK_DIR/inspect.out" '^packet 1: tag 6 (public key), length 525$' "pgpmsg inspect did not list the public-key packet"
assert_file_contains "$WORK_DIR/inspect.out" 'tag 2 (signature)' "pgpmsg inspect did not list signature packets"
assert_file_contains "$WORK_DIR/inspect.out" 'tag 14 (public subkey)' "pgpmsg inspect did not list subkey packets"

"${TEST_BIN_DIR}/pgpmsg" --json inspect "$SAMPLE_KEY" > "$WORK_DIR/inspect.jsonl"
assert_file_contains "$WORK_DIR/inspect.jsonl" '"schema":"newos.tool.v1"' "pgpmsg --json inspect did not use the shared JSON envelope"
assert_file_contains "$WORK_DIR/inspect.jsonl" '"event":"packet"' "pgpmsg --json inspect did not emit packet events"
assert_file_contains "$WORK_DIR/inspect.jsonl" '"name":"public key"' "pgpmsg --json inspect did not name public-key packets"

"${TEST_BIN_DIR}/pgpmsg" inspect -v "$ENCRYPTED_MESSAGE" > "$WORK_DIR/inspect_verbose.out"
assert_file_contains "$WORK_DIR/inspect_verbose.out" '^input: ASCII armor$' "pgpmsg inspect -v did not describe ASCII armor input"
assert_file_contains "$WORK_DIR/inspect_verbose.out" '^  input-size: 1137 bytes$' "pgpmsg inspect -v did not report the armored input size"
assert_file_contains "$WORK_DIR/inspect_verbose.out" '^  armor-type: PGP MESSAGE$' "pgpmsg inspect -v did not report the armor type"
assert_file_contains "$WORK_DIR/inspect_verbose.out" '^  armor-body-lines: 17$' "pgpmsg inspect -v did not report armor body lines"
assert_file_contains "$WORK_DIR/inspect_verbose.out" '^  armor-base64-chars: 1060$' "pgpmsg inspect -v did not report armor base64 characters"
assert_file_contains "$WORK_DIR/inspect_verbose.out" '^  armor-crc24: 1e475c (verified)$' "pgpmsg inspect -v did not report the verified armor CRC24"
assert_file_contains "$WORK_DIR/inspect_verbose.out" '^  decoded-size: 793 bytes$' "pgpmsg inspect -v did not report decoded size"
assert_file_contains "$WORK_DIR/inspect_verbose.out" '^  header-offset: 0$' "pgpmsg inspect -v did not report packet header offsets"
assert_file_contains "$WORK_DIR/inspect_verbose.out" '^  body-offset: 3$' "pgpmsg inspect -v did not report packet body offsets"
assert_file_contains "$WORK_DIR/inspect_verbose.out" '^  packet-end-offset: 529$' "pgpmsg inspect -v did not report packet end offsets"
assert_file_contains "$WORK_DIR/inspect_verbose.out" '^  length-encoding: new two-octet definite$' "pgpmsg inspect -v did not report packet length encoding"
"${TEST_BIN_DIR}/pgpmsg" --json inspect -v "$ENCRYPTED_MESSAGE" > "$WORK_DIR/inspect_verbose.jsonl"
assert_file_contains "$WORK_DIR/inspect_verbose.jsonl" '"event":"input"' "pgpmsg --json inspect -v did not emit an input event"
assert_file_contains "$WORK_DIR/inspect_verbose.jsonl" '"armor_crc24":"1e475c"' "pgpmsg --json inspect -v did not report the armor CRC24"
assert_file_contains "$WORK_DIR/inspect_verbose.jsonl" '"header_offset":0' "pgpmsg --json inspect -v did not report packet offsets"

if "${TEST_BIN_DIR}/pgpmsg" verify "$SAMPLE_KEY" > "$WORK_DIR/verify.out" 2> "$WORK_DIR/verify.err"; then
    fail "pgpmsg verify reported cryptographic success before verification is implemented"
fi
assert_file_contains "$WORK_DIR/verify.out" '^signature: not checked$' "pgpmsg verify did not report signature metadata status"
assert_file_contains "$WORK_DIR/verify.out" '^issuer: 99d37c39fa2c23a8$' "pgpmsg verify did not report the signature issuer key ID"
assert_file_contains "$WORK_DIR/verify.err" 'cryptographic verification is not implemented yet' "pgpmsg verify did not explain the nonzero status"
else
    note "skipping external PGP message corpus checks"
fi

if [ ! -x "$PGPKEY_BIN" ]; then
    PGPKEY_BIN="$ROOT_DIR/build/host-macos-aarch64/pgpkey"
fi
"$PGPKEY_BIN" generate --userid "Message Signer <signer@example.com>" --out "$WORK_DIR/secret.asc" --public-out "$WORK_DIR/public.asc" --no-passphrase > "$WORK_DIR/keygen.out"
SIGNER_FPR=$(sed -n 's/^generated: //p' "$WORK_DIR/keygen.out" | head -1)
"$PGPKEY_BIN" generate --userid "Message Recipient <recipient@example.com>" --out "$WORK_DIR/recipient-secret.asc" --public-out "$WORK_DIR/recipient-public.asc" --no-passphrase > "$WORK_DIR/recipient-keygen.out"
RECIPIENT_FPR=$(sed -n 's/^generated: //p' "$WORK_DIR/recipient-keygen.out" | head -1)
assert_file_contains "$WORK_DIR/keygen.out" '^profile: rfc9580$' "pgpkey generate did not use the RFC 9580 default profile for pgpmsg tests"
"${TEST_BIN_DIR}/pgpmsg" sign --detach --armor -s "$WORK_DIR/secret.asc" -u "$SIGNER_FPR" -o "$WORK_DIR/plain.sig.asc" "$WORK_DIR/plain.txt"
assert_file_contains "$WORK_DIR/plain.sig.asc" '^-----BEGIN PGP SIGNATURE-----$' "pgpmsg sign did not write an armored detached signature"
"${TEST_BIN_DIR}/pgpmsg" sign --detach --armor -s "$WORK_DIR/secret.asc" -u 'signer@example.com' -o "$WORK_DIR/plain-email.sig.asc" "$WORK_DIR/plain.txt"
"${TEST_BIN_DIR}/pgpmsg" verify -k "$WORK_DIR/public.asc" "$WORK_DIR/plain-email.sig.asc" "$WORK_DIR/plain.txt" > "$WORK_DIR/verify_email_signer.out"
assert_file_contains "$WORK_DIR/verify_email_signer.out" '^signature: good$' "pgpmsg sign did not accept a user ID signer selector"
"${TEST_BIN_DIR}/pgpmsg" sign --cleartext -s "$WORK_DIR/secret.asc" -u "$SIGNER_FPR" -o "$WORK_DIR/plain.clear.asc" "$WORK_DIR/plain.txt"
assert_file_contains "$WORK_DIR/plain.clear.asc" '^-----BEGIN PGP SIGNED MESSAGE-----' "pgpmsg sign --cleartext did not write a cleartext signed message"
assert_file_contains "$WORK_DIR/plain.clear.asc" '^Hash: SHA512' "pgpmsg sign --cleartext did not advertise the SHA512 hash"
assert_file_contains "$WORK_DIR/plain.clear.asc" '^-----BEGIN PGP SIGNATURE-----$' "pgpmsg sign --cleartext did not include an armored signature block"
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
if grep 'tag 8 (compressed data)' "$WORK_DIR/encrypted.inspect" >/dev/null 2>&1; then
    fail "pgpmsg encrypt compressed by default"
fi
assert_file_contains "$WORK_DIR/encrypted.inspect" '^  pkesk-version: 6$' "pgpmsg inspect did not decode the v6 PKESK version"
assert_file_contains "$WORK_DIR/encrypted.inspect" '^  public-key-algorithm: X25519$' "pgpmsg inspect did not decode the v6 PKESK algorithm"
assert_file_contains "$WORK_DIR/encrypted.inspect" 'tag 18 (symmetrically encrypted integrity protected data)' "pgpmsg encrypt did not write an integrity-protected encrypted data packet"
assert_file_contains "$WORK_DIR/encrypted.inspect" '^  seipd-version: 2$' "pgpmsg inspect did not decode the v2 SEIPD version"
assert_file_contains "$WORK_DIR/encrypted.inspect" '^  symmetric-algorithm: AES-256$' "pgpmsg inspect did not decode the v2 SEIPD cipher"
assert_file_contains "$WORK_DIR/encrypted.inspect" '^  aead-algorithm: GCM$' "pgpmsg inspect did not decode the v2 SEIPD AEAD algorithm"
"${TEST_BIN_DIR}/pgpmsg" decrypt -s "$WORK_DIR/secret.asc" -o "$WORK_DIR/plain.dec" "$WORK_DIR/plain.pgp.asc"
cmp "$WORK_DIR/plain.txt" "$WORK_DIR/plain.dec" || fail "pgpmsg decrypt did not recover the original plaintext"
"${TEST_BIN_DIR}/pgpmsg" encrypt --sign -s "$WORK_DIR/secret.asc" -u 'signer@example.com' -k "$WORK_DIR/public.asc" -r "$SIGNER_FPR" --armor -o "$WORK_DIR/plain-signed.pgp.asc" "$WORK_DIR/plain.txt"
assert_file_contains "$WORK_DIR/plain-signed.pgp.asc" '^-----BEGIN PGP MESSAGE-----$' "pgpmsg encrypt --sign did not write an armored message"
"${TEST_BIN_DIR}/pgpmsg" decrypt -k "$WORK_DIR/public.asc" -s "$WORK_DIR/secret.asc" -o "$WORK_DIR/plain-signed.dec" "$WORK_DIR/plain-signed.pgp.asc" > "$WORK_DIR/plain-signed-decrypt.out" 2> "$WORK_DIR/plain-signed-decrypt.err"
assert_file_contains "$WORK_DIR/plain-signed-decrypt.err" 'embedded signature: good' "pgpmsg decrypt did not verify an embedded signature"
cmp "$WORK_DIR/plain.txt" "$WORK_DIR/plain-signed.dec" || fail "pgpmsg decrypt did not recover an encrypted signed message"
"${TEST_BIN_DIR}/pgpmsg" encrypt -k "$WORK_DIR/public.asc" -r "$SIGNER_FPR" -o "$WORK_DIR/plain-v2.pgp" "$WORK_DIR/plain.txt"
tamper_last_byte "$WORK_DIR/plain-v2.pgp" "$WORK_DIR/plain-v2-tampered.pgp"
if "${TEST_BIN_DIR}/pgpmsg" decrypt -s "$WORK_DIR/secret.asc" -o "$WORK_DIR/plain-v2-tampered.dec" "$WORK_DIR/plain-v2-tampered.pgp" > "$WORK_DIR/plain-v2-tampered.out" 2> "$WORK_DIR/plain-v2-tampered.err"; then
    fail "pgpmsg decrypt accepted a tampered v2 SEIPD message"
fi
assert_file_contains "$WORK_DIR/plain-v2-tampered.err" 'decryption failed' "pgpmsg decrypt did not reject a tampered v2 SEIPD message"

if "${TEST_BIN_DIR}/pgpmsg" encrypt --stream -k "$WORK_DIR/public.asc" -r "$SIGNER_FPR" -o "$WORK_DIR/plain-stream-v6.pgp" "$WORK_DIR/plain.txt" > "$WORK_DIR/plain-stream-v6.out" 2> "$WORK_DIR/plain-stream-v6.err"; then
    fail "pgpmsg encrypt --stream accepted an RFC 9580 v6 recipient"
fi
assert_file_contains "$WORK_DIR/plain-stream-v6.err" 'legacy-v4 only' "pgpmsg encrypt --stream did not explain rejected RFC 9580 recipient"

"$PGPKEY_BIN" generate --legacy-v4 --userid "Legacy Recipient <legacy@example.com>" --out "$WORK_DIR/legacy-secret.asc" --public-out "$WORK_DIR/legacy-public.asc" --no-passphrase > "$WORK_DIR/legacy-keygen.out"
LEGACY_FPR=$(sed -n 's/^generated: //p' "$WORK_DIR/legacy-keygen.out" | head -1)

awk 'BEGIN { for (i = 0; i < 9000; i++) print "streaming pgp line " i }' > "$WORK_DIR/plain-stream.txt"
"${TEST_BIN_DIR}/pgpmsg" encrypt --stream -k "$WORK_DIR/legacy-public.asc" -r "$LEGACY_FPR" -o "$WORK_DIR/plain-stream.pgp" "$WORK_DIR/plain-stream.txt"
STREAM_PACKET_PREFIX=$(dd if="$WORK_DIR/plain-stream.pgp" bs=1 skip=96 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')
if [ "$STREAM_PACKET_PREFIX" != "d2f0" ]; then
    fail "pgpmsg encrypt --stream did not emit a partial-length SEIPD packet"
fi
"${TEST_BIN_DIR}/pgpmsg" inspect "$WORK_DIR/plain-stream.pgp" > "$WORK_DIR/plain-stream.inspect"
assert_file_contains "$WORK_DIR/plain-stream.inspect" 'tag 18 (symmetrically encrypted integrity protected data)' "pgpmsg inspect did not normalize a streamed encrypted packet"
"${TEST_BIN_DIR}/pgpmsg" decrypt -s "$WORK_DIR/legacy-secret.asc" -o "$WORK_DIR/plain-stream.dec" "$WORK_DIR/plain-stream.pgp"
cmp "$WORK_DIR/plain-stream.txt" "$WORK_DIR/plain-stream.dec" || fail "pgpmsg decrypt did not recover a streamed encrypted message"
tamper_last_byte "$WORK_DIR/plain-stream.pgp" "$WORK_DIR/plain-stream-tampered.pgp"
if "${TEST_BIN_DIR}/pgpmsg" decrypt -s "$WORK_DIR/legacy-secret.asc" -o "$WORK_DIR/plain-stream-tampered.dec" "$WORK_DIR/plain-stream-tampered.pgp" > "$WORK_DIR/plain-stream-tampered.out" 2> "$WORK_DIR/plain-stream-tampered.err"; then
    fail "pgpmsg decrypt accepted a tampered v1 SEIPD message"
fi
assert_file_contains "$WORK_DIR/plain-stream-tampered.err" 'decryption failed' "pgpmsg decrypt did not reject a tampered v1 SEIPD message"
if "${TEST_BIN_DIR}/pgpmsg" encrypt --stream --armor -k "$WORK_DIR/legacy-public.asc" -r "$LEGACY_FPR" -o "$WORK_DIR/plain-stream-armored.pgp" "$WORK_DIR/plain.txt" > "$WORK_DIR/plain-stream-armored.out" 2> "$WORK_DIR/plain-stream-armored.err"; then
    fail "pgpmsg encrypt --stream accepted --armor"
fi
assert_file_contains "$WORK_DIR/plain-stream-armored.err" 'stream does not support --armor yet' "pgpmsg encrypt --stream did not explain rejected armor"
if "${TEST_BIN_DIR}/pgpmsg" encrypt --stream --compress=zlib -k "$WORK_DIR/legacy-public.asc" -r "$LEGACY_FPR" -o "$WORK_DIR/plain-stream-compressed.pgp" "$WORK_DIR/plain.txt" > "$WORK_DIR/plain-stream-compressed.out" 2> "$WORK_DIR/plain-stream-compressed.err"; then
    fail "pgpmsg encrypt --stream accepted compression"
fi
assert_file_contains "$WORK_DIR/plain-stream-compressed.err" 'stream currently supports only --compress=none' "pgpmsg encrypt --stream did not explain rejected compression"

if [ "$external_pgp_fixtures_available" -eq 1 ]; then
"$PGPKEY_BIN" -k "$WORK_DIR/raimond-ring.pgp" import "$RAIMOND_KEY" > "$WORK_DIR/raimond-import.out"
"${TEST_BIN_DIR}/pgpmsg" encrypt -k "$WORK_DIR/raimond-ring.pgp" -r 'raimond.spekking@gmail.com' --armor -o "$WORK_DIR/raimond.pgp.asc" "$WORK_DIR/plain.txt"
assert_file_contains "$WORK_DIR/raimond.pgp.asc" '^-----BEGIN PGP MESSAGE-----$' "pgpmsg encrypt did not write an armored Elgamal-recipient message"
"${TEST_BIN_DIR}/pgpmsg" inspect "$WORK_DIR/raimond.pgp.asc" > "$WORK_DIR/raimond.inspect"
assert_file_contains "$WORK_DIR/raimond.inspect" 'tag 1 (public-key encrypted session key)' "pgpmsg Elgamal encryption did not write a public-key encrypted session key packet"
assert_file_contains "$WORK_DIR/raimond.inspect" '^  pkesk-version: 3$' "pgpmsg inspect did not decode the legacy PKESK version"
assert_file_contains "$WORK_DIR/raimond.inspect" '^  recipient-key-id: 556fc2fb94738613$' "pgpmsg inspect did not decode the Elgamal recipient key ID"
assert_file_contains "$WORK_DIR/raimond.inspect" '^  public-key-algorithm: Elgamal encrypt-only$' "pgpmsg inspect did not decode the Elgamal PKESK algorithm"
assert_file_contains "$WORK_DIR/raimond.inspect" '^  encrypted-mpi 1: [0-9][0-9]* bits, [0-9][0-9]* bytes$' "pgpmsg inspect did not summarize the first Elgamal MPI"
assert_file_contains "$WORK_DIR/raimond.inspect" '^  encrypted-mpi 2: [0-9][0-9]* bits, [0-9][0-9]* bytes$' "pgpmsg inspect did not summarize the second Elgamal MPI"
assert_file_contains "$WORK_DIR/raimond.inspect" 'tag 18 (symmetrically encrypted integrity protected data)' "pgpmsg Elgamal encryption did not write an integrity-protected encrypted data packet"
assert_file_contains "$WORK_DIR/raimond.inspect" '^  seipd-version: 1$' "pgpmsg inspect did not decode the legacy SEIPD version"
else
    note "skipping external PGP Elgamal recipient checks"
fi

"${TEST_BIN_DIR}/pgpmsg" encrypt -k "$WORK_DIR/public.asc" -r "$SIGNER_FPR" --compress=zlib --armor -o "$WORK_DIR/plain-zlib.pgp.asc" "$WORK_DIR/plain.txt"
"${TEST_BIN_DIR}/pgpmsg" decrypt -s "$WORK_DIR/secret.asc" -o "$WORK_DIR/plain-zlib.dec" "$WORK_DIR/plain-zlib.pgp.asc"
cmp "$WORK_DIR/plain.txt" "$WORK_DIR/plain-zlib.dec" || fail "pgpmsg decrypt did not recover a zlib-compressed encrypted message"

if [ "$external_pgp_fixtures_available" -eq 1 ]; then
"${TEST_BIN_DIR}/pgpmsg" encrypt -k "$SAMPLE_KEY" -r 'info@freiheitsrechte.org' --armor -o "$WORK_DIR/rsa.pgp.asc" "$WORK_DIR/plain.txt"
assert_file_contains "$WORK_DIR/rsa.pgp.asc" '^-----BEGIN PGP MESSAGE-----$' "pgpmsg encrypt did not write an armored RSA-recipient message"
"${TEST_BIN_DIR}/pgpmsg" inspect "$WORK_DIR/rsa.pgp.asc" > "$WORK_DIR/rsa.inspect"
assert_file_contains "$WORK_DIR/rsa.inspect" '^  pkesk-version: 3$' "pgpmsg inspect did not decode the RSA PKESK version"
assert_file_contains "$WORK_DIR/rsa.inspect" '^  recipient-key-id: 27467a158c2ecddc$' "pgpmsg inspect did not decode the RSA recipient key ID"
assert_file_contains "$WORK_DIR/rsa.inspect" '^  public-key-algorithm: RSA encrypt/sign$' "pgpmsg inspect did not decode the RSA PKESK algorithm"
assert_file_contains "$WORK_DIR/rsa.inspect" '^  encrypted-mpi 1: [0-9][0-9]* bits, [0-9][0-9]* bytes$' "pgpmsg inspect did not summarize the RSA encrypted session key MPI"
assert_file_contains "$WORK_DIR/rsa.inspect" '^  seipd-version: 1$' "pgpmsg RSA encryption did not use the legacy SEIPD envelope"
else
    note "skipping external PGP RSA recipient checks"
fi

SUBKEY_FPR=$("$PGPKEY_BIN" show "$WORK_DIR/legacy-public.asc" | sed -n 's/^subkey-fingerprint: //p' | head -1)
"$PGPKEY_BIN" edit "$WORK_DIR/legacy-secret.asc" --out "$WORK_DIR/revoked-secret.asc" --public-out "$WORK_DIR/revoked-public.asc" --revoke-subkey "$SUBKEY_FPR" > "$WORK_DIR/revoke-subkey.out"
if "${TEST_BIN_DIR}/pgpmsg" encrypt -k "$WORK_DIR/revoked-public.asc" -r "$LEGACY_FPR" -o "$WORK_DIR/revoked-blocked.pgp" "$WORK_DIR/plain.txt" > "$WORK_DIR/revoked-blocked.out" 2> "$WORK_DIR/revoked-blocked.err"; then
    fail "pgpmsg encrypt accepted a revoked encryption subkey"
fi
assert_file_contains "$WORK_DIR/revoked-blocked.err" 'no matching supported encryption subkey with encryption usage flags' "pgpmsg encrypt did not explain rejected encryption usage flags"
"${TEST_BIN_DIR}/pgpmsg" encrypt -k "$WORK_DIR/revoked-public.asc" -r "$LEGACY_FPR" --DANGER-anyway -o "$WORK_DIR/revoked-override.pgp" "$WORK_DIR/plain.txt"
"${TEST_BIN_DIR}/pgpmsg" decrypt -s "$WORK_DIR/revoked-secret.asc" -o "$WORK_DIR/revoked-override.dec" "$WORK_DIR/revoked-override.pgp"
cmp "$WORK_DIR/plain.txt" "$WORK_DIR/revoked-override.dec" || fail "pgpmsg --DANGER-anyway override did not produce decryptable output"

"$PGPKEY_BIN" -k "$WORK_DIR/pubring.pgp" import "$WORK_DIR/public.asc" "$WORK_DIR/recipient-public.asc" > "$WORK_DIR/pubring-import.out"
env NEWOS_PGPMSG_WORKERS=4 "${TEST_BIN_DIR}/pgpmsg" encrypt -k "$WORK_DIR/pubring.pgp" -r "$SIGNER_FPR" -r "$RECIPIENT_FPR" --armor -o "$WORK_DIR/plain-multi.pgp.asc" "$WORK_DIR/plain.txt"
"${TEST_BIN_DIR}/pgpmsg" inspect "$WORK_DIR/plain-multi.pgp.asc" > "$WORK_DIR/encrypted-multi.inspect"
PKESK_COUNT=$(grep -c 'tag 1 (public-key encrypted session key)' "$WORK_DIR/encrypted-multi.inspect")
if [ "$PKESK_COUNT" -ne 2 ]; then
    fail "pgpmsg encrypt did not write one session-key packet per recipient"
fi
"${TEST_BIN_DIR}/pgpmsg" decrypt -s "$WORK_DIR/secret.asc" -o "$WORK_DIR/plain-multi-signer.dec" "$WORK_DIR/plain-multi.pgp.asc"
cmp "$WORK_DIR/plain.txt" "$WORK_DIR/plain-multi-signer.dec" || fail "pgpmsg decrypt did not recover a multi-recipient message with the first secret key"
"${TEST_BIN_DIR}/pgpmsg" decrypt -s "$WORK_DIR/recipient-secret.asc" -o "$WORK_DIR/plain-multi-recipient.dec" "$WORK_DIR/plain-multi.pgp.asc"
cmp "$WORK_DIR/plain.txt" "$WORK_DIR/plain-multi-recipient.dec" || fail "pgpmsg decrypt did not recover a multi-recipient message with the second secret key"

awk 'BEGIN { for (i = 0; i < 90000; ++i) printf "parallel pgpmsg line %05d abcdefghijklmnopqrstuvwxyz 0123456789\n", i }' > "$WORK_DIR/plain-parallel-v2.txt"
env NEWOS_PGPMSG_WORKERS=4 "${TEST_BIN_DIR}/pgpmsg" encrypt -k "$WORK_DIR/public.asc" -r "$SIGNER_FPR" -o "$WORK_DIR/plain-parallel-v2.pgp" "$WORK_DIR/plain-parallel-v2.txt"
env NEWOS_PGPMSG_WORKERS=4 "${TEST_BIN_DIR}/pgpmsg" decrypt -s "$WORK_DIR/secret.asc" -o "$WORK_DIR/plain-parallel-v2.dec" "$WORK_DIR/plain-parallel-v2.pgp"
cmp "$WORK_DIR/plain-parallel-v2.txt" "$WORK_DIR/plain-parallel-v2.dec" || fail "pgpmsg parallel v2 AEAD round trip did not recover the original plaintext"
