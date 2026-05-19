# traceroute(1)

## Name

traceroute - trace the route to a host with increasing ICMP TTL values

## Synopsis

`traceroute [-4|-6] [-n] [-m MAXTTL] [-q QUERIES] [-w SECONDS] HOST`

## Description

`traceroute` sends ICMP echo probes with increasing TTL values from 1 up to `MAXTTL`. It prints each responding hop, shows `*` for probes that time out, and stops when a probe reaches the destination. By default, it tries to show reverse-DNS host names for responding hops and falls back to numeric addresses when no name is available.

The platform backend decodes ICMP time-exceeded replies and echo replies directly for IPv4 and IPv6, so traceroute output is concise and does not include ping banners or ping statistics.

## Options

- `-4` use IPv4 probes
- `-6` use IPv6 probes
- `-n` skip reverse-DNS lookups and print numeric addresses only
- `-m MAXTTL` set the maximum TTL, default 30
- `-q QUERIES` send `QUERIES` probes per TTL, default 1
- `-w SECONDS` wait up to `SECONDS` for each probe, default 1
- `-h`, `--help` show usage information

## Examples

`traceroute 127.0.0.1`

`traceroute -m 5 -q 1 example.com`

`traceroute -n -6 example.com`

## Limitations

IPv4 and IPv6 hop reporting is supported on POSIX, macOS freestanding, and Linux freestanding backends. IPv6 availability still depends on the host having IPv6 routing and on the platform allowing ICMPv6 sockets. Host names depend on reverse-DNS records and resolver availability.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

