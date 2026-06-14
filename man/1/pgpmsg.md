# PGPMSG

## NAME

pgpmsg - inspect OpenPGP messages and expose message crypto commands

## SYNOPSIS

```
pgpmsg [--json] COMMAND [ARGS...]

pgpmsg inspect [FILE]
pgpmsg verify [-k PUBRING] SIGNATURE [FILE]
pgpmsg encrypt -r RECIPIENT [-k PUBRING] [-o OUT] [--armor] [FILE]
pgpmsg decrypt [-s SECRING] [-o OUT] [FILE]
pgpmsg sign -u SIGNER [-s SECRING] [-o OUT] [--armor] [--detach|--cleartext] [FILE]
```

## DESCRIPTION

`pgpmsg` is the OpenPGP message companion to `pgpkey`. `pgpkey` manages and
inspects public certificates and local public keyrings; `pgpmsg` is the place for
message and file operations such as encryption, decryption, signing, and
signature verification.

The current implementation provides packet inspection and signature packet
metadata reporting. Cryptographic encryption, decryption, signing, and signature
validation are intentionally not enabled yet. Commands that would require those
operations fail explicitly instead of producing non-OpenPGP output or making
trust claims.

## COMMANDS

- `inspect [FILE]` - decode binary or ASCII-armored OpenPGP input and list packet tags and body lengths. With no FILE or `-`, read standard input.
- `verify [-k PUBRING] SIGNATURE [FILE]` - decode SIGNATURE, report signature packet metadata, and return a nonzero status because cryptographic validation is not implemented yet. FILE is accepted for the detached-signature command shape.
- `encrypt -r RECIPIENT [-k PUBRING] [-o OUT] [--armor] [FILE]` - reserved for public-key encryption. This command currently fails with an explicit unsupported-operation diagnostic.
- `decrypt [-s SECRING] [-o OUT] [FILE]` - reserved for private-key decryption. This command currently fails with an explicit unsupported-operation diagnostic.
- `sign -u SIGNER [-s SECRING] [-o OUT] [--armor] [--detach|--cleartext] [FILE]` - reserved for detached, binary, and cleartext signatures. This command currently fails with an explicit unsupported-operation diagnostic.

## OPTIONS

- `--json` - emit shared JSON Lines events where supported.
- `-k PUBRING`, `--keyring PUBRING` - public keyring path for future verification and encryption.
- `-s SECRING`, `--secring SECRING` - secret keyring path for future signing and decryption.
- `-r RECIPIENT`, `--recipient RECIPIENT` - recipient selector for future encryption.
- `-u SIGNER`, `--user SIGNER`, `--signer SIGNER` - signer selector for future signing.
- `-o OUT`, `--output OUT` - output path for future message-writing commands.
- `--armor` - request ASCII-armored output for future message-writing commands.
- `--detach` - request a detached signature for future signing.
- `--cleartext` - request a cleartext signed message for future signing.
- `-h`, `--help` - show usage.

## VERIFY OUTPUT

`verify` prints the signature packet type, public-key algorithm, hash algorithm,
signature creation date when present, issuer key ID, issuer fingerprint when
present, and `trust: not evaluated`. The status is currently `not checked`.
This is packet metadata only; it is not a cryptographic verification result.

## JSON OUTPUT

`inspect --json` emits `packet` events. `verify --json` emits `signature` events
with `status: not_checked` plus decoded signature metadata.

## EXIT STATUS

`0` is used for successful inspection. `1` is used for syntax and input errors.
`2` is used when a command shape is recognized but the required cryptographic
operation is not implemented yet.

## SEE ALSO

pgpkey, pgpquery
