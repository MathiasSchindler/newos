# IP

## NAME

ip - inspect and adjust network links, addresses, and routes

## SYNOPSIS

```sh
ip [-4|-6] [-br|-brief|-o|--oneline] address [show [IFACE|dev IFACE]]
ip [-4|-6] address {add|del} ADDRESS/PREFIX dev IFACE
ip [-4|-6] address flush dev IFACE
ip [-br|-brief|-o|--oneline] link [show [IFACE|dev IFACE]]
ip link set dev IFACE [up|down] [mtu N]
ip [-4|-6] route [show [default|PREFIX|dev IFACE]]
ip route {add|del} DESTINATION[/PREFIX]|default [via GATEWAY] [dev IFACE]
```

## DESCRIPTION

`ip` provides a compact, Linux-first interface for inspecting and changing host
network settings. It focuses on the most common day-to-day tasks:

- list interfaces and their state
- show IPv4 and IPv6 addresses
- show brief single-line interface summaries
- show IPv4 and IPv6 route tables on Linux and hosted POSIX/macOS builds
- bring links up or down
- adjust MTU values
- add, remove, or flush IPv4 addresses and routes when the host kernel allows it

## OPTIONS

- `-4` - limit display or changes to IPv4-oriented data
- `-6` - limit display or changes to IPv6-oriented data
- `-br`, `-brief` - print concise one-line summaries
- `-o`, `--oneline` - compatibility alias for compact output
- `-h`, `--help` - show the command usage summary

## OBJECTS

- `address`, `addr`, `a`
  - `show [IFACE|dev IFACE]` - display interface addresses
  - `flush dev IFACE` - remove matching IPv4 addresses from an interface
  - `add ADDRESS/PREFIX dev IFACE` - set an IPv4 address and netmask
  - `del ADDRESS/PREFIX dev IFACE` - remove an IPv4 address

- `link`, `l`
  - `show [IFACE|dev IFACE]` - display link state, MTU, and link-layer address
  - `set dev IFACE up|down [mtu N]` - change common link attributes

- `route`, `r`
  - `show [default|PREFIX|dev IFACE]` - display the current route table, optionally filtered by destination or device
  - `add DESTINATION|default [via GATEWAY] [dev IFACE]` - add an IPv4 route
  - `del DESTINATION|default [via GATEWAY] [dev IFACE]` - remove an IPv4 route

## LIMITATIONS

- This is not a full `iproute2` replacement.
- Hosted route display now works across Linux and common POSIX/macOS builds, but advanced route metadata is intentionally compact.
- Address and route mutation still focus on IPv4.
- Changing interfaces or routes typically requires elevated privileges.
- Advanced policy routing, monitoring, namespaces, tunnels, and JSON output are
  not implemented.

## EXAMPLES

```sh
ip addr
ip -br addr
ip -4 addr show dev eth0
ip link show eth0
ip link set dev eth0 up
ip link set dev eth0 mtu 1400
ip route show default
ip -6 route show
ip addr add 192.168.10.20/24 dev eth0
ip addr flush dev eth0
ip route add default via 192.168.10.1 dev eth0
```

## SEE ALSO

ping, netcat, hostname, wget
