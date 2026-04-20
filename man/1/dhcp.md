# DHCP

## NAME

dhcp - acquire a small IPv4 DHCP lease

## SYNOPSIS

```text
dhcp [-A] [-i IFACE] [-s SERVER] [-p SERVER_PORT] [-P CLIENT_PORT] [-t TIMEOUT]
```

## DESCRIPTION

`dhcp` requests an IPv4 lease from a DHCP server and prints the assigned
address, gateway, DNS servers, and lease lifetime. With `-A` it also applies
the leased address and default route to the chosen interface.

## CURRENT CAPABILITIES

- perform a small DHCP discover/request/ack exchange
- print the leased address, router, DNS servers, and lease time
- target a specific server and custom test ports for debugging or local tests
- optionally apply the address and default route with `-A`
- work in hosted builds and in the freestanding Linux environment

## OPTIONS

- `-A` - apply the acquired address and default route to `IFACE`
- `-i IFACE` - interface to use for apply mode or interface-specific probing
- `-s SERVER` - DHCP server address; useful for controlled or test setups
- `-p SERVER_PORT` - DHCP server port (default: `67`)
- `-P CLIENT_PORT` - local client port (default: `68`)
- `-t TIMEOUT` - lease timeout such as `2s` or `500ms`
- `-h`, `--help` - show a short usage summary

## LIMITATIONS

- this is currently a small IPv4 DHCP client rather than a full lease manager
- renew, release, background daemon mode, and persistent lease storage are not
  implemented yet
- automatic resolver-file updates are not written by the tool today
- applying a lease usually requires elevated privileges and a usable interface

## EXAMPLES

```text
dhcp

# print a lease without changing interface state
dhcp -s 10.0.2.2 -t 3s

# acquire and apply a lease to eth0
dhcp -A -i eth0
```

## SEE ALSO

ip, ping, nslookup
