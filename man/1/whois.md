# whois(1)

## Name

whois - query a WHOIS server

## Synopsis

`whois [-R] [-h SERVER] [-p PORT] [-w SECONDS] [--json] QUERY`

## Description

`whois` connects to a WHOIS server, sends `QUERY` followed by CRLF, and writes the server response to standard output.

The default server is `whois.iana.org` on TCP port 43. When the response names a referral server in a `refer:`, `whois:`, or `ReferralServer:` field, `whois` follows that referral and prints the registry response as well. This gives the more detailed allocation, contact, and registry data that users usually expect for IP address queries. Referral targets are accepted only when they look like plain WHOIS host names; malformed values with control characters, schemes other than `whois://`, paths, ports, or other punctuation are ignored.

## Options

- `-R` do not follow referral servers
- `-h SERVER` query `SERVER` instead of `whois.iana.org`
- `-p PORT` connect to `PORT` instead of 43
- `-w SECONDS` wait up to `SECONDS` for response data before failing or ending an idle response; the default is 10 seconds
- `--json` write newline-delimited JSON events
- `--help` show usage information

## Examples

`whois example.com`

`whois -h whois.iana.org com`

`whois -R 193.99.144.85`

## Limitations

Referral following is intentionally shallow and only follows plain WHOIS server names. Use `-h` to select a specific registry server directly, or `-R` to inspect only the first server response. Response capture for referral detection is bounded; very large WHOIS responses continue streaming to output but only the retained prefix is scanned for referrals.

## JSON Output

With `--json`, `whois` writes JSON Lines using the common envelope documented in `json-output`. Each server query starts with a `whois_query_start` event whose `data` contains `server`, `port`, and `query`.

Response bytes are streamed as `whois_response_chunk` events as they arrive. Each chunk contains `server`, `port`, `query`, `bytes`, and `text`. When a server finishes, `whois_query_complete` reports `captured_bytes`, which is the retained response size used for referral detection. If referral following is enabled, each referred server produces its own start, chunk, and complete sequence.

