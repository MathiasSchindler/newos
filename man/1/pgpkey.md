# PGPKEY

## NAME

pgpkey - inspect OpenPGP keys and maintain a simple public keyring

## SYNOPSIS

```
pgpkey [-k KEYRING] [-v] [--color[=WHEN]|--no-color] [--json] COMMAND [ARGS...]

pgpkey show [-v] [FILE ...]
pgpkey packets FILE ...
pgpkey import FILE ...
pgpkey list
pgpkey export FINGERPRINT [OUTPUT]
```

## DESCRIPTION

`pgpkey` reads binary OpenPGP certificate data or ASCII-armored public key
blocks. It can inspect public keys, list packet tags, import public
certificates into an append-only keyring, list the keyring, and export a
matching certificate as an ASCII-armored public key block.

The keyring is intentionally simple: it is a concatenation of normalized binary
OpenPGP certificate packets. It is not a trust database and does not assign
validity. Secret-key storage, key generation, encryption, signing, and signature
verification belong to later OpenPGP tools.

With no command, `pgpkey` behaves like `pgpkey show` and reads the default
keyring.

## COMMANDS

- `show [-v] [FILE ...]` - print certificate summaries for files, or the default keyring when no file is given.
- `packets FILE ...` - print packet tags and packet lengths.
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

## KEYRING

The default keyring path is taken from `PGPKEY_KEYRING` when set. Otherwise,
`pgpkey` uses `$HOME/.pgpkeyring`. The parent directory must already exist.

Fingerprints may be written as continuous hex, colon-separated hex, or spaced
hex. For lookup, a full v4 fingerprint or the trailing 64-bit key ID is accepted.

## JSON Output

When `--json` is used, `pgpkey show` and `pgpkey list` emit `certificate` events
with fingerprint, key ID, algorithm, user IDs, subkey count, signature count,
and decoded signature-metadata count. `pgpkey import` emits `import` events for
newly imported certificates.

## SEE ALSO

base64, sha1sum, mail