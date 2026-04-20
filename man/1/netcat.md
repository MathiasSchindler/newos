# NETCAT

## NAME

netcat - arbitrary TCP/UDP connections and listeners

## SYNOPSIS

```
netcat [-46nuvz] [-w TIMEOUT] [-s ADDR] [-p PORT] [-v] HOST PORT
netcat -l [-46kuv] [-w TIMEOUT] [-s ADDR] [-p PORT] [ADDR] PORT
```

## DESCRIPTION

`netcat` opens a TCP (or UDP with `-u`) connection to HOST:PORT and relays
data between the connection and standard input/output. With `-l` it listens
for an incoming connection instead.

## CURRENT CAPABILITIES

- Outbound TCP connection to HOST PORT
- Listen for a single incoming TCP connection with -l
- Keep a listener alive for multiple connections with -k
- Port-scan mode with -z
- Configurable connection timeout with -w
- Bind or source-select a local address and port with -s and -p
- IPv4 operation across hosted and freestanding Linux builds
- Additional UDP and IPv6 modes on backends that implement them
- Verbose reporting of connection events with -v

## OPTIONS

- `-4` — force IPv4 sockets
- `-6` — force IPv6 sockets where the backend supports it
- `-l` — listen mode; wait for an incoming connection on PORT
- `-k` — keep listening for multiple connections or datagrams
- `-u` — use UDP instead of TCP
- `-z` — scan mode; check whether the port is open without sending data
- `-n` — numeric addresses only; skip name resolution
- `-s ADDR` — bind the local socket or listener to ADDR
- `-p PORT` — bind the local socket to PORT before connecting or listening
- `-w TIMEOUT` — set connection or idle timeout (accepts suffix `s`, `ms`,
  `m`; bare number is milliseconds, so values such as `250ms` or `1.5s` work)
- `-v` — verbose output

## LIMITATIONS

- UDP and IPv6 support depend on the selected platform backend; the hosted POSIX build has the strongest coverage today, while the freestanding Linux backend is currently IPv4 and TCP focused.
- No -e execute program or -c shell command mode.
- No UNIX-domain socket support.

## EXAMPLES

```
netcat example.com 80
echo "GET / HTTP/1.0\r\n" | netcat example.com 80
netcat -w 250ms example.com 80
netcat -l -k -s 127.0.0.1 8080
netcat -u 224.0.0.1 5353
netcat -4 -n -z -w 1s 127.0.0.1 22
netcat -z -w 1s host.local 22
```

## SEE ALSO

ping
