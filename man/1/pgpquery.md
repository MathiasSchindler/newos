# PGPQUERY

## NAME

pgpquery - query public OpenPGP keyservers

## SYNOPSIS

```
pgpquery [--server NAME|URL] [--json] [--get] [-o OUT|--import-keyring KEYRING] [--print-url] [--timeout DURATION] SELECTOR...
```

## DESCRIPTION

`pgpquery` performs read-only lookups against public OpenPGP keyservers. It is a
networked companion to `pgpkey`: `pgpquery` can discover public certificates and
keyserver index metadata, while `pgpkey` remains the local key inspection and
keyring tool.

Selectors may be 64-bit key IDs, full fingerprints, or email addresses. Hex key
selectors may be written as continuous hex, `0x`-prefixed hex, colon-separated
hex, or spaced hex; requests use normalized lowercase hex.

The default server mode queries both `keys.openpgp.org` and
`keyserver.ubuntu.com` when the selector can be represented on both services.
Additional servers such as Mailvelope, MIT, and WKD are available explicitly
with `--server`, but are not included in the default fan-out. No trust,
ownership, revocation, or signature-validity claims are made. A public
keyserver response only says that a server returned data for a selector.

## OPTIONS

- `-s NAME`, `--server NAME` - choose a server. NAME may be `all`, `openpgp`, `keys.openpgp.org`, `ubuntu`, `keyserver.ubuntu.com`, `mailvelope`, `keys.mailvelope.com`, `mit`, `pgp.mit.edu`, `wkd`, or an `http://` or `https://` base URL.
- `--json` - emit JSON Lines events instead of human-readable text.
- `--get` - fetch armored certificate data from an HKP server and write clean importable key material to standard output. With `all` or `openpgp`, this currently selects `keyserver.ubuntu.com` because `keys.openpgp.org` has separate lookup paths.
- `-o OUT`, `--output OUT` - with `--get`, write the fetched certificate to OUT instead of standard output. This option accepts exactly one selector.
- `--import-keyring KEYRING` - with `--get`, import fetched certificates directly into a `pgpkey` binary keyring, reporting `imported` or `unchanged` fingerprints.
- `--print-url` - print the URL or URLs that would be queried, without using the network.
- `-T DURATION`, `--timeout DURATION` - set the plain HTTP socket timeout. Durations use the shared tool format, such as `5s` or `1000ms`. HTTPS/TLS queries currently use the platform TLS client's built-in timeout instead of this option.
- `-h`, `--help` - show usage.

## SERVERS

`keys.openpgp.org` is queried with the VKS API:

```
https://keys.openpgp.org/vks/v1/by-keyid/KEYID
https://keys.openpgp.org/vks/v1/by-fingerprint/FINGERPRINT
https://keys.openpgp.org/vks/v1/by-email/EMAIL
```

Successful VKS responses are parsed as OpenPGP certificates and summarized with
fingerprint, key ID, and user IDs when present.

`keyserver.ubuntu.com` is queried with HKP machine-readable index mode by
default:

```
https://keyserver.ubuntu.com/pks/lookup?op=index&options=mr&search=SELECTOR
```

With `--get`, the HKP `op=get` endpoint is used instead and the returned armored
certificate is written directly to standard output, OUT, or KEYRING. HTTP
chunked-transfer framing is decoded before output, and fetched certificates are
checked for importable OpenPGP public-key material.

`keys.mailvelope.com` and `pgp.mit.edu` are available as explicit HKP-style
servers:

```
https://keys.mailvelope.com/pks/lookup?op=index&options=mr&search=SELECTOR
https://pgp.mit.edu/pks/lookup?op=index&options=mr&search=SELECTOR
```

With `--get`, these servers also use HKP `op=get`.

`wkd` performs Web Key Directory lookup for email selectors. It prints or tries
the advanced method first, followed by the direct method:

```
https://openpgpkey.DOMAIN/.well-known/openpgpkey/DOMAIN/hu/HASH?l=LOCAL
https://DOMAIN/.well-known/openpgpkey/hu/HASH?l=LOCAL
```

`HASH` is the WKD z-base-32 SHA-1 hash of the lowercase email local-part. WKD
does not accept key IDs or fingerprints.

Custom servers are treated as simple base URLs. The selector is URL-encoded and
appended to the base URL, with a slash inserted when the base URL does not end
in `/` or `=`.

## JSON Output

When `--json` is used, `pgpquery` emits shared `newos.tool.v1` JSON Lines
events. Event names include `query_url`, `certificate`, `hkp_index`, and
`hkp_uid`, depending on the server and operation.

## FETCH AND IMPORT

For a manual two-step workflow, use `--get -o FILE` and then inspect or import
FILE with `pgpkey`:

```
pgpquery --server ubuntu --get -o alice.asc KEYID
pgpkey show alice.asc
pgpkey -k pubring.pgp import alice.asc
```

For a direct fetch/import workflow, use `--import-keyring`:

```
pgpquery --server ubuntu --get --import-keyring pubring.pgp KEYID...
```

The keyring format is the same append-only binary keyring maintained by
`pgpkey import`.

## PRIVACY

Public keyserver lookups disclose the selector to the contacted server and to
normal network intermediaries. Use `--print-url` to inspect requests before
sending them.

## SEE ALSO

pgpkey, wget, mail
