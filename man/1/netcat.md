# NETCAT

## NAME

netcat - arbitrary TCP/UDP connections and listeners

## SYNOPSIS

```
netcat [-u] [-z] [-w TIMEOUT] [-v] HOST PORT
netcat -l [-u] [-w TIMEOUT] [-v] PORT
```

## DESCRIPTION

`netcat` opens a TCP (or UDP with `-u`) connection to HOST:PORT and relays
data between the connection and standard input/output. With `-l` it listens
for an incoming connection instead.

## CURRENT CAPABILITIES

- Outbound TCP connection to HOST PORT
- Listen for a single incoming TCP connection with `-l`
- UDP mode with `-u`
- Port-scan mode (connect and immediately close) with `-z`
- Configurable connection timeout with `-w`
- Verbose reporting of connection events with `-v`

## OPTIONS

- `-l` — listen mode; wait for an incoming connection on PORT
- `-u` — use UDP instead of TCP
- `-z` — scan mode; check whether the port is open without sending data
- `-w TIMEOUT` — set connection or idle timeout (accepts suffix `s`, `ms`,
  `m`; bare number is milliseconds, so values such as `250ms` or `1.5s` work)
- `-v` — verbose output

## LIMITATIONS

- Only a single connection per invocation; no `-k` (keep listening).
- Listen mode binds by port only; there is no flag for choosing a specific
  local address or source port.
- No `-e` (execute program) or `-c` (shell command).
- No UNIX-domain socket support.
- No IPv6 flag; address family is determined by the platform resolver.

## EXAMPLES

```
netcat example.com 80
echo "GET / HTTP/1.0\r\n" | netcat example.com 80
netcat -w 250ms example.com 80
netcat -l 8080
netcat -u 224.0.0.1 5353
netcat -z -w 1s host.local 22
```

## SEE ALSO

ping
