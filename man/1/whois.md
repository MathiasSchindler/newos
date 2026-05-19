# whois(1)

## Name

whois - query a WHOIS server

## Synopsis

`whois [-R] [-h SERVER] [-p PORT] QUERY`

## Description

`whois` connects to a WHOIS server, sends `QUERY` followed by CRLF, and writes the server response to standard output.

The default server is `whois.iana.org` on TCP port 43. When the response names a referral server in a `refer:`, `whois:`, or `ReferralServer:` field, `whois` follows that referral and prints the registry response as well. This gives the more detailed allocation, contact, and registry data that users usually expect for IP address queries.

## Options

- `-R` do not follow referral servers
- `-h SERVER` query `SERVER` instead of `whois.iana.org`
- `-p PORT` connect to `PORT` instead of 43
- `--help` show usage information

## Examples

`whois example.com`

`whois -h whois.iana.org com`

`whois -R 193.99.144.85`

## Limitations

Referral following is intentionally shallow and only follows plain WHOIS server names. Use `-h` to select a specific registry server directly, or `-R` to inspect only the first server response.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

