# NSLOOKUP

## NAME

nslookup - look up IPv4 and IPv6 addresses for a host name

## SYNOPSIS

```text
nslookup [-4|-6] [-s SERVER] [-p PORT] NAME
```

## DESCRIPTION

`nslookup` performs a small DNS-style address lookup for `NAME` and prints the
resolved addresses. It is intended as a compact userland inspection tool rather
than a full diagnostic suite.

## CURRENT CAPABILITIES

- resolve IPv4 `A` records
- resolve IPv6 `AAAA` records
- use the default platform resolver when no server override is given
- query a specific numeric DNS server with `-s` and `-p`
- work in hosted builds and in the freestanding Linux environment

## OPTIONS

- `-4` - limit the lookup to IPv4 answers
- `-6` - limit the lookup to IPv6 answers
- `-s SERVER` - query the specified DNS server instead of the default resolver
- `-p PORT` - use the specified DNS port instead of `53`
- `-h`, `--help` - show a short usage summary

## LIMITATIONS

- this is a small address-lookup tool, not a full `dig` replacement
- output currently focuses on address answers rather than the full DNS message
- advanced record types, reverse lookups, and detailed flags are not yet shown
- freestanding lookups work best with numeric name servers or the guest's
  default resolver configuration

## EXAMPLES

```text
nslookup localhost
nslookup -6 example.com
nslookup -s 1.1.1.1 example.com
```

## SEE ALSO

ip, ping, netcat, wget
