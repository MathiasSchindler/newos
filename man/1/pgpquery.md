# PGPQUERY

## NAME

pgpquery - query public OpenPGP keyservers

## SYNOPSIS

```
pgpquery [--server NAME|URL] [--json] [--get] [--print-url] [--timeout DURATION] SELECTOR...
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
No trust, ownership, revocation, or signature-validity claims are made. A public
keyserver response only says that a server returned data for a selector.

## OPTIONS

- `-s NAME`, `--server NAME` - choose a server. NAME may be `all`, `openpgp`, `keys.openpgp.org`, `ubuntu`, `keyserver.ubuntu.com`, or an `http://` or `https://` base URL.
- `--json` - emit JSON Lines events instead of human-readable text.
- `--get` - fetch armored certificate data from an HKP server and write it to standard output. With `all` or `openpgp`, this currently selects `keyserver.ubuntu.com` because `keys.openpgp.org` has separate lookup paths.
- `--print-url` - print the URL or URLs that would be queried, without using the network.
- `-T DURATION`, `--timeout DURATION` - set the plain TCP socket timeout. Durations use the shared tool format, such as `5s` or `1000ms`.
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
certificate is written directly to standard output.

Custom servers are treated as simple base URLs. The selector is URL-encoded and
appended to the base URL, with a slash inserted when the base URL does not end
in `/` or `=`.

## JSON OUTPUT

When `--json` is used, `pgpquery` emits shared `newos.tool.v1` JSON Lines
events. Event names include `query_url`, `certificate`, `hkp_index`, and
`hkp_uid`, depending on the server and operation.

## PRIVACY

Public keyserver lookups disclose the selector to the contacted server and to
normal network intermediaries. Use `--print-url` to inspect requests before
sending them.

## SEE ALSO

pgpkey, wget, mail
