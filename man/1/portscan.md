# portscan(1)

## Name

portscan - check TCP ports on authorized hosts

## Synopsis

`portscan [-46an] [-w TIMEOUT] [--common] [--services] [--summary] [--progress] [--banner] HOSTS [PORTS...]`

## Description

`portscan` checks whether TCP ports accept normal connect attempts. It is intended for inventory, troubleshooting, and hygiene checks on systems you own or are explicitly authorized to test.

The tool does not perform raw packet scans, spoofing, stealth probes, service exploitation, credential attempts, or vulnerability detection. A reported `open` port only means that a TCP connection completed.

By default, only open ports are printed. Use `-a` to print closed results too.

`HOSTS` may be a single host, a comma-separated list, or an IPv4 last-octet range such as `192.0.2.1-5`. `PORTS` may be one or more arguments, each containing a single port, a comma-separated list, or a range such as `22,80,443` or `8000-8010`. Use `--common` to scan a conservative set of common administration and service ports without listing ports explicitly.

Options may appear before or after `HOSTS` and `PORTS`. For example, `portscan router.lan --common --summary` and `portscan --common --summary router.lan` are equivalent.

With `--banner`, after each successful connect `portscan` passively reads up to `--banner-bytes` bytes that the service volunteers within `--banner-timeout`. No data is sent, no protocol handshake is performed, and no probe is written to the wire. The intent is to identify what is listening on an unexpected port without active service interrogation.

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
- `--progress` print every completed result as it is scanned, including non-open states that would normally be hidden unless `-a` is used
- `--csv` write CSV rows with `host,port,state,service` columns
- `--fail-open` exit with status 2 when any open port is found
- `--fail-closed` exit with status 3 when any closed port is found
- `--banner` after connect, read whatever the service volunteers; print it as an additional column with non-printable bytes escaped. Nothing is sent to the service.
- `--banner-bytes N` maximum banner bytes to capture; default `256`, hard cap `1024`
- `--banner-timeout TIME` how long to wait for banner data after connect; default `500ms`
- `-h`, `--help` show usage information

## Examples

`portscan localhost 22,80,443`

`portscan -a -w 500ms 127.0.0.1 1-1024`

`portscan 192.0.2.10,192.0.2.20 22 80 443`

`portscan 192.0.2.1-5 8000-8010`

`portscan --common --services --summary router.lan`

`portscan router.lan --common --summary --progress`

`portscan --csv -a 127.0.0.1 22,80,443`

`portscan --fail-open 10.0.0.25 23,3389`

`portscan --banner --services 127.0.0.1 22,25,80,143`

## Output

Each result is printed as:

```text
HOST PORT open
HOST PORT closed
HOST PORT filtered
HOST PORT unreachable
HOST PORT error
```

Non-open results are only printed with `-a` or `--progress`. `closed` means the connection was actively refused. `filtered` means the connection timed out or remained in progress until the platform gave up. `unreachable` means name resolution, the host, or the network was unreachable. `error` is used for failures that cannot be classified portably.

`--progress` uses the same line format as normal output, but prints each completed port check as soon as it finishes. This is useful for common-port or range scans where otherwise nothing may appear until an open port is found or the final summary is printed.

With `--services`, a service-name hint is appended when the port has a built-in well-known entry. With `--banner`, an additional escaped banner field is appended whenever a banner was captured:

```text
HOST PORT open ssh SSH-2.0-OpenSSH_9.6p1\r\n
HOST PORT open http
```

The banner field is omitted from a line when no bytes were received within the timeout, so a quiet open port still prints as `HOST PORT open`.

With `--csv`, output starts with a header and then prints one row per displayed result. When `--banner` is also set, the header gains a `banner` column and each row includes the escaped banner text (empty for closed ports or quiet services):

```text
host,port,state,service
127.0.0.1,22,open,ssh
```

```text
host,port,state,service,banner
127.0.0.1,22,open,ssh,SSH-2.0-OpenSSH_9.6p1\r\n
127.0.0.1,80,open,http,
```

With `--summary`, the final line is:

```text
summary scanned=COUNT open=COUNT closed=COUNT filtered=COUNT unreachable=COUNT error=COUNT
```

### Banner escaping

Bytes outside printable ASCII are escaped before output so that arbitrary remote data cannot inject terminal control sequences or break CSV parsing. The escape rules are:

- `\\` for a literal backslash
- `\t`, `\n`, `\r`, `\0` for tab, newline, carriage return, and NUL
- `\xHH` (lowercase hex) for any other byte below `0x20` or above `0x7e`

Banners are read once with a single `recv` and truncated at `--banner-bytes`; no second read is attempted.

## Exit Status

The normal exit status is 0 when the scan completed and 1 for invalid arguments or scan setup errors. `--fail-open` changes the exit status to 2 when any open port is found. `--fail-closed` changes the exit status to 3 when any actively refused `closed` port is found.

## Limitations

Only TCP connect checks are implemented. UDP scanning is not included because lack of a UDP response is ambiguous: the service may be open but silent, a firewall may have dropped the probe, or an ICMP error may have been filtered. Future UDP support should be an explicit mode and should document that silence is inconclusive.

Timeout handling depends on the platform networking backend. Local and refused connections usually complete quickly; filtered network paths may take longer on backends that cannot interrupt a blocking connect.

Connect failures are classified from platform error details where available. The mapping is intentionally small and portable: refused connections are `closed`, timeouts and still-in-progress connects are `filtered`, and name, host, or network reachability failures are `unreachable`. Some platform backends may still report unusual failures as `error` when the native error cannot be mapped safely.

Service names are static hints based on common port numbers. `portscan` does not perform active protocol detection, so a service hint does not prove what software is actually listening. The optional `--banner` mode reads bytes a service volunteers on its own; it sends nothing, does not speak any protocol, and does not attempt version-to-CVE mapping. A banner is whatever the service chose to send and may be misleading or empty.

Reverse-DNS display for numeric host ranges, JSON output, and reading host or port lists from files are not implemented yet. These are diagnostic conveniences rather than scan-behavior changes and can be added later without changing the conservative TCP-connect model.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

