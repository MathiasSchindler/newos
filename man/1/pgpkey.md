# PGPKEY

## NAME

pgpkey - inspect OpenPGP keys and maintain a simple public keyring

## SYNOPSIS

```
pgpkey [-k KEYRING] [-v] [--color[=WHEN]|--no-color] [--json] COMMAND [ARGS...]

pgpkey show [-v] [FILE ...]
pgpkey packets FILE ...
pgpkey issuers [--external] [FILE ...]
pgpkey catalog-sql [FILE ...]
pgpkey generate --userid USERID --out SECRET.asc --public-out PUBLIC.asc --no-passphrase [--expires DURATION] [--profile rfc9580|legacy-v4]
pgpkey edit SECRET.asc --out SECRET.asc [--public-out PUBLIC.asc] OPERATION
pgpkey import FILE ...
pgpkey list
pgpkey export FINGERPRINT [OUTPUT]
```

## DESCRIPTION

`pgpkey` reads binary OpenPGP certificate data, ASCII-armored public key blocks,
and ASCII-armored private key blocks. It can inspect keys, list packet tags,
generate a small RFC 9580 key pair, import public certificates into an append-only
keyring, list the keyring, export a matching certificate as an ASCII-armored
public key block, and perform append-preserving edits on generated unprotected
secret keys.

The keyring is intentionally simple: it is a concatenation of normalized binary
OpenPGP certificate packets. It is not a trust database and does not assign
validity. Generated secret keys are written as separate private key blocks, not
imported into the public keyring.

With no command, `pgpkey` behaves like `pgpkey show` and reads the default
keyring.

## COMMANDS

- `show [-v] [FILE ...]` - print certificate summaries for files, or the default keyring when no file is given.
- `packets FILE ...` - print packet tags and packet lengths.
- `issuers [--external] [FILE ...]` - list unique signature issuer key IDs found in certificates, optionally excluding issuer IDs already present as primary keys or subkeys in the same input set.
- `catalog-sql [FILE ...]` - emit SQL statements that rebuild a local metadata catalog for the given certificates, or for the default keyring when no file is given.
- `generate --userid USERID --out SECRET.asc --public-out PUBLIC.asc --no-passphrase [--expires DURATION] [--profile rfc9580|legacy-v4]` - generate an Ed25519 OpenPGP secret key and matching public certificate. The default profile is RFC 9580 v6; `legacy-v4` is retained for compatibility.
- `edit SECRET.asc --out SECRET.asc [--public-out PUBLIC.asc] OPERATION` - append a key edit signature or generated subkey to an unprotected secret key and optionally write the matching public certificate.
- `import FILE ...` - decode public key files and append new certificates to the keyring.
- `list` - show certificates in the keyring.
- `export FINGERPRINT [OUTPUT]` - write a matching certificate as ASCII-armored public key data to standard output or OUTPUT.

## OPTIONS

- `-k KEYRING`, `--keyring KEYRING` - use KEYRING instead of the default.
- `-v`, `--verbose` - for `show`, include per-signature metadata.
- `--color[=WHEN]` - color status text. WHEN is `auto`, `always`, or `never`. With no value, `auto` is used.
- `--no-color` - disable color output. This is equivalent to `--color=never`.
- `--json` - emit JSON Lines events for `show`, `list`, and `import`.
- `-h`, `--help` - show usage.

## GENERATE

`pgpkey generate` defaults to an RFC 9580 v6 profile: an Ed25519 primary key
that can certify and sign, plus an X25519 encryption subkey. It writes two
ASCII-armored files: an unencrypted private key block and a matching public key
block. Both outputs must be file paths; standard output is refused for generated
key material. The public certificate includes a Direct Key self-signature, a user
ID certification, and a subkey binding signature with creation time, issuer
fingerprint metadata, key flags, algorithm preferences, AEAD preferences,
feature flags, primary-UID marker, and key expiration metadata.

The `legacy-v4` profile remains available with `--profile legacy-v4`,
`--profile v4`, or `--legacy-v4`. It emits the older v4 EdDSA legacy primary key
and ECDH Curve25519/X25519 subkey shape used by earlier versions of this tool and
many deployed OpenPGP implementations. New keys should use the RFC 9580 default;
legacy v4 key material is marked as deprecated by RFC 9580.

Required options are:

- `--userid USERID`, `--user-id USERID`, or `-u USERID` - set the OpenPGP user ID, usually `Name <mail@example.com>`.
- `--out SECRET.asc` or `--secret-out SECRET.asc` - write the private key block.
- `--public-out PUBLIC.asc` - write the public certificate.
- `--no-passphrase` - explicitly acknowledge that the private key is not passphrase-protected.

Optional generation options are:

- `--expires DURATION` - set the key expiration interval. DURATION is a number of seconds or a number followed by `s`, `d`, `w`, `m`, or `y`; use `never`, `none`, or `0` for no expiration.
- `--algorithm ed25519` - select the only implemented generation algorithm.
- `--profile rfc9580|legacy-v4` - select the generated certificate profile. Aliases `v6` and `v4` are accepted.
- `--legacy-v4` - shorthand for `--profile legacy-v4`.
- `--armor` - accepted for clarity; generated output is always ASCII-armored.

Passphrase-protected secret-key packets, S2K encryption, full key revocation
certificates, and trust assignment are not implemented yet. Without
`--no-passphrase`, generation refuses to run so unprotected private keys cannot
be created accidentally.

## EDIT

`pgpkey edit` operates on the first certificate in an unprotected secret key file
created by `pgpkey generate`. It preserves the original packet bytes and inserts
or appends new OpenPGP packets. Use `--public-out PUBLIC.asc` to also write a
public certificate with secret-key packets converted to public-key packets.

Exactly one edit operation is accepted per invocation:

- `--add-uid USERID` - append a new user ID packet and a positive self-signature.
- `--revoke-uid USERID` - insert a certification revocation signature for an existing user ID.
- `--set-primary-uid USERID` - insert a fresh positive self-signature that marks an existing user ID as primary.
- `--change-expiration DURATION` or `--expires DURATION` - refresh the primary user ID self-signature with a new key-expiration interval.
- `--refresh-self-signatures` - refresh all user ID self-signatures and subkey binding signatures with current preferences, usage flags, and expiration metadata.
- `--add-subkey` - generate and append a new unprotected X25519/ECDH encryption subkey with encryption usage flags.
- `--revoke-subkey FINGERPRINT` - insert a subkey revocation signature for a matching subkey fingerprint or key ID.

The edit path currently signs with the Ed25519 primary key and supports only the
unprotected legacy-v4 key shape generated by this tool. It does not yet edit RFC
9580 v6 certificates, passphrase-protected keys, create full primary-key
revocation certificates, or rewrite third-party certifications.

## SHOW OUTPUT

The default `show` output includes the primary key, fingerprint, key ID, user
IDs, primary user ID marker, key and subkey usage flags, key and subkey
expiration dates when present, expiration status, algorithm preferences,
subkeys, and packet counts. Expiration status is printed as text, such as
`(not expired)` or `(expired)`, and is colored green or red when color output is
enabled.

With `-v`, `show` also prints one line for each decoded signature packet. The
verbose signature lines include the signature type, signature creation date,
public-key and hash algorithms, target UID or subkey, issuer key ID, issuer
fingerprint when present, signature expiration date and interval, key
expiration date and interval, key flags, and primary-UID markers. Signature
cryptographic validation is not performed yet; these lines describe packet
metadata.

Color follows the shared tool convention. In `auto` mode, color is used only
for suitable terminals and respects `NO_COLOR`, `CLICOLOR`, `CLICOLOR_FORCE`,
and `TERM=dumb`.

Algorithm preference lists also use color when enabled. Broken or unsafe hash
algorithms such as MD5, SHA-1, and RIPEMD-160 are highlighted red. Legacy
choices such as SHA-224, IDEA, TripleDES, and CAST5 are highlighted yellow.
Compression preferences are printed without security coloring because they are
compatibility and side-channel considerations rather than broken cryptographic
algorithms.

## KEYRING

The default keyring path is taken from `PGPKEY_KEYRING` when set. Otherwise,
`pgpkey` uses `$HOME/.pgpkeyring`. The parent directory must already exist.

Fingerprints may be written as continuous hex, colon-separated hex, or spaced
hex. For lookup, a full fingerprint or the 64-bit key ID is accepted. V4 key IDs
are the trailing eight fingerprint octets; v6 key IDs are the first eight
fingerprint octets.

## ISSUERS

`pgpkey issuers FILE...` scans decoded signature metadata and prints one unique
issuer per line. The first column is always the 64-bit issuer key ID. When an
issuer fingerprint subpacket is present, the full fingerprint is printed as a
second column.

`--external` suppresses issuer key IDs that are already present as primary keys
or subkeys in the same input set. This is useful when building an experimental
keychain from referenced third-party certifiers:

```
pgpkey issuers --external *.asc
```

## SQL CATALOG

`pgpkey catalog-sql FILE...` emits a rebuild script for the project-local `sql`
tool. The script creates and clears four tables: `certs`, `keys`, `user_ids`,
and `signatures`, then inserts metadata decoded from the supplied certificates.
When no FILE is given, the default keyring is used.

The normalized OpenPGP keyring or source certificate files remain the source of
truth. The SQL catalog is derived data for fast local lookup and inspection. A
typical experimental rebuild is:

```
pgpkey catalog-sql experimental/pgp-keys/*.asc experimental/pgp-keys/*.txt | sql experimental/pgp-keys/pgpkeys.sqs
```

Issuer names can be resolved when the issuer certificate is present in the same
catalog:

```
sql experimental/pgp-keys/pgpkeys.sqs "SELECT signatures.issuer_key_id, certs.primary_uid FROM signatures LEFT JOIN certs ON signatures.issuer_key_id = certs.key_id;"
```

Resolved user IDs are local labels, not trust decisions. Keep fingerprints and
key IDs visible when displaying resolved issuer information.

## JSON Output

When `--json` is used, `pgpkey show` and `pgpkey list` emit `certificate` events
with fingerprint, key ID, algorithm, user IDs, subkey count, signature count,
and decoded signature-metadata count. `pgpkey issuers` emits `issuer` events
with key IDs and issuer fingerprints when available. `pgpkey import` emits
`import` events for newly imported certificates. `pgpkey generate` emits one
`generate` event with the generated profile, version, fingerprint, key ID, user
ID, output paths, algorithm, curve, and protection status.

## SEE ALSO

base64, sha1sum, mail