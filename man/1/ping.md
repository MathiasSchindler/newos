# PING

## NAME

ping - send ICMP ECHO_REQUEST packets to a host

## SYNOPSIS

```
ping [-c COUNT] [-i SECONDS] [-W SECONDS] [-s BYTES] [-t TTL] HOST
```

## DESCRIPTION

`ping` sends ICMP echo request packets to HOST and reports round-trip time
and packet loss statistics.

## CURRENT CAPABILITIES

- Send a configurable number of packets
- Configurable send interval, timeout, payload size, and TTL
- Reports per-packet RTT and a summary on completion
- Hostname resolution through the platform IPv4 networking backend

## OPTIONS

- `-c COUNT` — send COUNT packets (must be ≥ 1; default: platform value)
- `-i SECONDS` — interval between packets in seconds (default: platform value)
- `-W SECONDS` — per-packet reply wait timeout in seconds (must be ≥ 1;
  default: platform value)
- `-s BYTES` — payload size in bytes (default: platform value; max: platform max)
- `-t TTL` — IP time-to-live (0 uses OS default; max: platform max)

## LIMITATIONS

- Requires appropriate privileges (raw socket or setuid) to send ICMP packets.
- No IPv6 support; IPv4 only.
- Timing and size options are integer-only; fractional intervals are not accepted.
- Flood mode (`-f`) is not implemented.
- Deadline mode (`-w`) is not implemented.

## EXAMPLES

```
ping example.com
ping -c 4 192.168.1.1
ping -c 5 -i 2 -W 3 host.local
ping -s 64 -t 64 10.0.0.1
```

## SEE ALSO

netcat
