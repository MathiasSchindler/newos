# HOST

## NAME

host - simple DNS lookup utility

## SYNOPSIS

```
host [-4|-6] [-t TYPE] [-s SERVER] [--json] NAME [TYPE]
```

## DESCRIPTION

`host` is a compact DNS lookup frontend built on the existing platform DNS
backend. It prints address, name-server, mail-exchanger, and text records in a
short human-readable form.

Supported record types are `A`, `AAAA`, `MX`, `NS`, and `TXT`.

The shared DNS parser rejects malformed replies such as reserved label types,
bad compression pointers, and truncated record data before answers are printed.

## OPTIONS

- `-4` - query IPv4 A records.
- `-6` - query IPv6 AAAA records.
- `-t TYPE` - select a record type.
- `-s SERVER` - query a specific DNS server.
- `--json` - emit JSON Lines events instead of human-readable answers.
- `-h`, `--help` - show usage.

## JSON Output

With `--json`, `host` emits one `answer` event per DNS answer:

```json
{"schema":"newos.tool.v1","tool":"host","stream":"stdout","event":"answer","seq":1,"data":{"query":"example.com","name":"example.com","type":"A","ttl":300,"preference":0,"data":"93.184.216.34"}}
```

Diagnostics and usage messages follow the shared `json-output` envelope.

## SEE ALSO

dig, nslookup, ping, wget
