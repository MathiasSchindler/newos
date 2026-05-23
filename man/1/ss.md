# SS

## NAME

ss - show socket state

## SYNOPSIS

```
ss [-t] [-u] [-l] [-a] [--json]
```

## DESCRIPTION

`ss` lists TCP and UDP sockets through the platform socket-listing API. On Linux
freestanding builds this reads kernel socket tables exposed through `/proc/net`.

## OPTIONS

- `-t`, `--tcp` - show TCP sockets.
- `-u`, `--udp` - show UDP sockets.
- `-l`, `--listening` - show listening sockets only.
- `-a`, `--all` - show all matching sockets.
- `--json` - emit JSON Lines events instead of the text table.
- `-h`, `--help` - show usage.

## JSON Output

With `--json`, `ss` emits one `socket` event per socket:

```json
{"schema":"newos.tool.v1","tool":"ss","stream":"stdout","event":"socket","seq":1,"data":{"protocol":"tcp","state":"LISTEN","local_address":"127.0.0.1","local_port":8080,"remote_address":"0.0.0.0","remote_port":0,"inode":12345}}
```

Diagnostics and usage messages follow the shared `json-output` envelope.

## LIMITATIONS

The first implementation focuses on Linux `/proc/net` data. IPv6 addresses may
be shown in kernel hexadecimal form rather than compressed presentation form.
Process name/PID ownership and advanced filters are not implemented yet.

## SEE ALSO

ip, netcat, lsof, ps
