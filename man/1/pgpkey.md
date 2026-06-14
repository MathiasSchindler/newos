# PGPKEY

## NAME

pgpkey - inspect OpenPGP keys and maintain a simple public keyring

## SYNOPSIS

```
pgpkey [-k KEYRING] [-v] [--color[=WHEN]|--no-color] [--json] COMMAND [ARGS...]

pgpkey show [-v] [FILE ...]
pgpkey packets FILE ...
pgpkey generate --userid USERID --out SECRET.asc --public-out PUBLIC.asc --no-passphrase [--expires DURATION]
pgpkey import FILE ...
pgpkey list
pgpkey export FINGERPRINT [OUTPUT]
```

## DESCRIPTION

`pgpkey` reads binary OpenPGP certificate data, ASCII-armored public key blocks,
and ASCII-armored private key blocks. It can inspect keys, list packet tags,
generate a small modern key pair, import public certificates into an append-only
keyring, list the keyring, and export a matching certificate as an ASCII-armored
public key block.

The keyring is intentionally simple: it is a concatenation of normalized binary
OpenPGP certificate packets. It is not a trust database and does not assign
validity. Generated secret keys are written as separate private key blocks, not
imported into the public keyring.

With no command, `pgpkey` behaves like `pgpkey show` and reads the default
keyring.

## COMMANDS

- `show [-v] [FILE ...]` - print certificate summaries for files, or the default keyring when no file is given.
- `packets FILE ...` - print packet tags and packet lengths.
- `generate --userid USERID --out SECRET.asc --public-out PUBLIC.asc --no-passphrase [--expires DURATION]` - generate an Ed25519 OpenPGP secret key and matching public certificate.
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

`pgpkey generate` currently implements the first narrow key-generation path: an
Ed25519 primary key that can certify and sign. It writes two ASCII-armored
files: an unencrypted private key block and a matching public key block. Both
outputs must be file paths; standard output is refused for generated key
material. The public certificate includes a user ID and a self-signature with
creation time, issuer metadata, key flags, algorithm preferences, feature flags,
primary-UID marker, and key expiration metadata.

Required options are:

- `--userid USERID`, `--user-id USERID`, or `-u USERID` - set the OpenPGP user ID, usually `Name <mail@example.com>`.
- `--out SECRET.asc` or `--secret-out SECRET.asc` - write the private key block.
- `--public-out PUBLIC.asc` - write the public certificate.
- `--no-passphrase` - explicitly acknowledge that the private key is not passphrase-protected.

Optional generation options are:

- `--expires DURATION` - set the key expiration interval. DURATION is a number of seconds or a number followed by `s`, `d`, `w`, `m`, or `y`; use `never`, `none`, or `0` for no expiration.
- `--algorithm ed25519` - select the only implemented generation algorithm.
- `--armor` - accepted for clarity; generated output is always ASCII-armored.

Passphrase-protected secret-key packets, S2K encryption, encryption subkeys,
revocation certificates, and trust assignment are not implemented yet. Without
`--no-passphrase`, generation refuses to run so unprotected private keys cannot
be created accidentally.

## SHOW OUTPUT

The default `show` output includes the primary key, fingerprint, key ID, user
IDs, primary user ID marker, key and subkey usage flags, key and subkey
expiration dates when present, expiration status, algorithm preferences,
subkeys, and packet counts. Expiration status is printed as text, such as
`(valid)` or `(expired)`, and is colored green or red when color output is
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
hex. For lookup, a full v4 fingerprint or the trailing 64-bit key ID is accepted.

## JSON Output

When `--json` is used, `pgpkey show` and `pgpkey list` emit `certificate` events
with fingerprint, key ID, algorithm, user IDs, subkey count, signature count,
and decoded signature-metadata count. `pgpkey import` emits `import` events for
newly imported certificates. `pgpkey generate` emits one `generate` event with
the generated fingerprint, key ID, user ID, output paths, algorithm, curve, and
protection status.

## SEE ALSO

base64, sha1sum, mail