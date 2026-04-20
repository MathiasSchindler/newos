# PING

## NAME

ping - send ICMP ECHO_REQUEST packets to a host

## SYNOPSIS

```
ping [-4] [-nq] [-c COUNT] [-i SECONDS] [-W SECONDS] [-w DEADLINE] [-s BYTES] [-t TTL] HOST
```

## DESCRIPTION

`ping` sends ICMP echo request packets to HOST and reports round-trip time
and packet loss statistics.

## CURRENT CAPABILITIES

- Send a configurable number of packets
- Configurable send interval, timeout, payload size, and TTL
- Optional quiet mode and overall deadline handling
- Reports per-packet RTT and a summary on completion
- Hostname resolution through the platform IPv4 networking backend
- Accepts common compatibility flags such as `-4`, `-6`, and `-n`

## OPTIONS

- `-4` — force IPv4 mode (the current implementation default)
- `-6` — request IPv6 mode and fail with a clear diagnostic
- `-n` — numeric output only; accepted for compatibility
- `-q` — quiet output; suppress per-packet lines and show the summary block
- `-c COUNT` — send COUNT packets (must be ≥ 1; default: platform value)
- `-i SECONDS` — interval between packets in seconds (default: platform value)
- `-W SECONDS` — per-packet reply wait timeout in seconds (must be ≥ 1;
  default: platform value)
- `-w SECONDS` — stop after SECONDS overall, even if COUNT has not been reached
- `-s BYTES` — payload size in bytes (default: platform value; max: platform max)
- `-t TTL` — IP time-to-live (0 uses OS default; max: platform max)

## LIMITATIONS

- Requires appropriate privileges (raw socket or setuid) to send ICMP packets.
- IPv6 probes are not yet implemented; `-6` reports that explicitly.
- Timing and size options are integer-only; fractional intervals are not accepted.
- Flood mode (`-f`) is not implemented.

## EXAMPLES

```
ping example.com
ping -c 4 192.168.1.1
ping -q -c 5 host.local
ping -c 5 -i 2 -W 3 host.local
ping -c 10 -w 15 10.0.0.1
ping -s 64 -t 64 10.0.0.1
```

## SEE ALSO

netcat
