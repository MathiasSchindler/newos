# HOST

## NAME

host - simple DNS lookup utility

## SYNOPSIS

```
host [-4|-6] [-t TYPE] [-s SERVER] NAME [TYPE]
```

## DESCRIPTION

`host` is a compact DNS lookup frontend built on the existing platform DNS
backend. It prints address, name-server, mail-exchanger, and text records in a
short human-readable form.

Supported record types are `A`, `AAAA`, `MX`, `NS`, and `TXT`.

## OPTIONS

- `-4` - query IPv4 A records.
- `-6` - query IPv6 AAAA records.
- `-t TYPE` - select a record type.
- `-s SERVER` - query a specific DNS server.
- `-h`, `--help` - show usage.

## SEE ALSO

dig, nslookup, ping, wget
