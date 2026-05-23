# SS

## NAME

ss - show socket state

## SYNOPSIS

```
ss [-t] [-u] [-l] [-a]
```

## DESCRIPTION

`ss` lists TCP and UDP sockets through the platform socket-listing API. On Linux
freestanding builds this reads kernel socket tables exposed through `/proc/net`.

## OPTIONS

- `-t`, `--tcp` - show TCP sockets.
- `-u`, `--udp` - show UDP sockets.
- `-l`, `--listening` - show listening sockets only.
- `-a`, `--all` - show all matching sockets.
- `-h`, `--help` - show usage.

## LIMITATIONS

The first implementation focuses on Linux `/proc/net` data. IPv6 addresses may
be shown in kernel hexadecimal form rather than compressed presentation form.
Process name/PID ownership and advanced filters are not implemented yet.

## SEE ALSO

ip, netcat, lsof, ps
