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
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

nslookup, ping, ip, wget
