# traceroute(1)

## Name

traceroute - trace the route to a host with increasing ICMP TTL values

## Synopsis

`traceroute [-4|-6] [-n] [-m MAXTTL] [-q QUERIES] [-w SECONDS] [--json] HOST`

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
- `--json` write newline-delimited JSON events
- `-h`, `--help` show usage information

## Examples

`traceroute 127.0.0.1`

`traceroute -m 5 -q 1 example.com`

`traceroute -n -6 example.com`

## Limitations

IPv4 and IPv6 hop reporting is supported on POSIX, macOS freestanding, and Linux freestanding backends. IPv6 availability still depends on the host having IPv6 routing and on the platform allowing ICMPv6 sockets. Host names depend on reverse-DNS records and resolver availability.

## JSON Output

With `--json`, `traceroute` writes JSON Lines using the common envelope documented in `json-output`. The first event is `trace_start`, whose `data` contains `host`, `max_ttl`, `queries`, `timeout_seconds`, `family`, and `numeric_only`.

Each reported hop is emitted as a `trace_hop` event. Its `data` contains:

- `ttl`: hop TTL
- `address`: responding address, or `null`
- `hostname`: reverse-DNS host name, or `null`
- `reply_count`: number of probes that received replies
- `reached_destination`: boolean
- `probes`: an array of objects with `replied` and `rtt_ms`; `rtt_ms` is `null` for timed-out probes

Hop events are emitted as each TTL finishes, so callers can update progress while the trace is still running. Within a hop, the `probes` array is complete for that TTL.

