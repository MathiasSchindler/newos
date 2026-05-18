# portscan(1)

## Name

portscan - check TCP ports on authorized hosts

## Synopsis

`portscan [-46an] [-w TIMEOUT] [--common] [--services] [--summary] HOSTS [PORTS...]`

## Description

`portscan` checks whether TCP ports accept normal connect attempts. It is intended for inventory, troubleshooting, and hygiene checks on systems you own or are explicitly authorized to test.

The tool does not perform raw packet scans, spoofing, stealth probes, service exploitation, credential attempts, or vulnerability detection. A reported `open` port only means that a TCP connection completed.

By default, only open ports are printed. Use `-a` to print closed results too.

`HOSTS` may be a single host, a comma-separated list, or an IPv4 last-octet range such as `192.0.2.1-5`. `PORTS` may be one or more arguments, each containing a single port, a comma-separated list, or a range such as `22,80,443` or `8000-8010`. Use `--common` to scan a conservative set of common administration and service ports without listing ports explicitly.

## Options

- `-4` force IPv4
- `-6` force IPv6 where the platform supports it
- `-a` show closed ports as well as open ports
- `-n` numeric host names only; skip name resolution
- `-w TIMEOUT` connection timeout, using `ms`, `s`, or `m` suffixes; the default is `1s`
- `--common` scan a conservative set of common administration and service ports
- `--delay TIME` wait between connect attempts, using `ms`, `s`, or `m` suffixes
- `--services` show well-known service-name hints for common ports
- `--summary` print scanned, open, and closed totals after the scan
- `--csv` write CSV rows with `host,port,state,service` columns
- `--fail-open` exit with status 2 when any open port is found
- `--fail-closed` exit with status 3 when any closed port is found
- `-h`, `--help` show usage information

## Examples

`portscan localhost 22,80,443`

`portscan -a -w 500ms 127.0.0.1 1-1024`

`portscan 192.0.2.10,192.0.2.20 22 80 443`

`portscan 192.0.2.1-5 8000-8010`

`portscan --common --services --summary router.lan`

`portscan --csv -a 127.0.0.1 22,80,443`

`portscan --fail-open 10.0.0.25 23,3389`

## Output

Each result is printed as:

```text
HOST PORT open
HOST PORT closed
```

Closed results are only printed with `-a`.

With `--services`, a fourth field is appended when the port has a built-in well-known service hint:

```text
HOST PORT open ssh
HOST PORT closed https
```

With `--csv`, output starts with a header and then prints one row per displayed result:

```text
host,port,state,service
127.0.0.1,22,open,ssh
```

With `--summary`, the final line is:

```text
summary scanned=COUNT open=COUNT closed=COUNT
```

## Exit Status

The normal exit status is 0 when the scan completed and 1 for invalid arguments or scan setup errors. `--fail-open` changes the exit status to 2 when any open port is found. `--fail-closed` changes the exit status to 3 when any closed port is found.

## Limitations

Only TCP connect checks are implemented. UDP scanning is not included because lack of a UDP response is ambiguous: the service may be open but silent, a firewall may have dropped the probe, or an ICMP error may have been filtered. Future UDP support should be an explicit mode and should document that silence is inconclusive.

Timeout handling depends on the platform networking backend. Local and refused connections usually complete quickly; filtered network paths may take longer on backends that cannot interrupt a blocking connect.

Closed, filtered, and unreachable failures are currently reported together as `closed` because the portable platform API only exposes connect success or failure. Distinguishing `closed/refused` from `filtered/timeout` would require preserving connection error detail in the platform layer.

Service names are static hints based on common port numbers. `portscan` does not perform banner grabbing or protocol detection, so a service hint does not prove what software is actually listening.

Reverse-DNS display for numeric host ranges, JSON output, and reading host or port lists from files are not implemented yet. These are diagnostic conveniences rather than scan-behavior changes and can be added later without changing the conservative TCP-connect model.