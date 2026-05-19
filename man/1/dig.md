# DIG

## NAME

dig - inspect a small subset of DNS records in a dig-style format

## SYNOPSIS

```
dig [-4|-6] [-t TYPE] [-s SERVER|@SERVER] [-p PORT] NAME [TYPE]
```

## DESCRIPTION

`dig` performs a compact DNS query and prints a `QUESTION SECTION` and
`ANSWER SECTION` similar to the classic BIND tool. This implementation is meant
for quick inspection and troubleshooting, not for full protocol analysis.

## CURRENT CAPABILITIES

- query `A`, `AAAA`, `MX`, `NS`, and `TXT` records
- accept `@SERVER` or `-s SERVER` to direct a query at a specific DNS server
- reuse the platform resolver for local/default `A` and `AAAA` lookups
- show `CNAME` records that appear in the answer set for supported lookups

## OPTIONS

- `-4` - force an IPv4 `A` lookup
- `-6` - force an IPv6 `AAAA` lookup
- `-t TYPE` - select the query type (`A`, `AAAA`, `MX`, `NS`, `TXT`)
- `-s SERVER` - query the specified DNS server
- `@SERVER` - dig-style shorthand for selecting a DNS server
- `-p PORT` - use the specified DNS port instead of `53`
- `--json` - write newline-delimited JSON events
- `-h`, `--help` - show a short usage summary

## LIMITATIONS

- this is a practical subset, not a full BIND-compatible `dig`
- output is focused on the answer section rather than full header/flag dumps
- record support is intentionally limited to the common types listed above
- numeric IPv4 server overrides are the most portable option across platforms

## EXAMPLES

```
dig localhost
dig -t AAAA localhost
dig @1.1.1.1 example.com MX
dig --json localhost
```

## JSON Output

With `--json`, `dig` writes JSON Lines using the common envelope documented in `json-output`. Modern automation engines can parse the single `dns_result` event written to standard output. Its `data` object contains:

- `query`: original domain name query string
- `type`: query request type (e.g. `A`, `AAAA`, `MX`, `NS`, `TXT`)
- `server`: custom override address, or `null`
- `port`: server port number (e.g., 53)
- `answers`: an array of resolved objects, containing:
  - `name`: record name
  - `type`: record answer type
  - `ttl`: answer Time-To-Live integer
  - `preference`: preference rank (for MX queries, 0 otherwise)
  - `data`: resolved address, target hostname, or payload text string

Diagnostics and query exceptions are mapped to the common `diagnostic` JSON Line object on standard error.

## SEE ALSO

nslookup, ping, ip, wget
