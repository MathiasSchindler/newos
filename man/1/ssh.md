# SSH

## NAME

ssh - minimal interactive SSH client

## SYNOPSIS

```
ssh [-v] [-i IDENTITY] [-l USER] [-p PORT] HOST
ssh [-v] [-i IDENTITY] [-l USER] [-p PORT] USER@HOST[:PORT] [PASSWORD]
```

## DESCRIPTION

`ssh` opens an interactive remote shell over SSH using the native `newos`
transport and crypto code. It keeps the current implementation intentionally
narrow: connect, verify the host key, authenticate, request a PTY, and hand
stdin/stdout through to a single shell session.

On first contact, the tool prints the remote host-key fingerprint and asks for
confirmation. Accepted keys are stored in `$HOME/.ssh/known_hosts`.

## CURRENT CAPABILITIES

- outbound SSH connections to IPv4 and IPv6 hosts
- destination parsing for `host`, `user@host`, `user@host:port`, and
  `user@[ipv6]:port`
- trust-on-first-use host-key pinning via `known_hosts`
- password authentication
- Ed25519 public-key authentication with `-i` or `~/.ssh/id_ed25519`
- interactive PTY shell sessions
- verbose transport/authentication progress with `-v`

## OPTIONS

- `-i IDENTITY` - use an unencrypted OpenSSH Ed25519 private key file, or a raw
  32-byte seed file
- `-l USER` - override the remote login name
- `-p PORT` - override the destination port
- `-v` - print connection and authentication progress
- `-h`, `--help` - show a short usage summary

## AUTHENTICATION AND TRUST

`ssh` currently tries methods in this order:

1. Ed25519 public-key authentication if a usable identity is available
2. password authentication if the server offers it

If no password is provided as the trailing positional argument, the tool prompts
on stdin.

If a stored host key no longer matches the server, the connection is aborted
instead of silently updating the trust record.

## LIMITATIONS

- The supported SSH profile is currently fixed to:
  - key exchange: `curve25519-sha256`
  - host keys: `ssh-ed25519`
  - transport cipher: `chacha20-poly1305@openssh.com`
  - compression: `none`
- No `ssh_config` parsing, `~/.ssh/config`, or command-line compatibility beyond the listed flags
- No agent support, passphrase-protected private keys, or RSA/ECDSA client keys
- No remote command execution mode, port forwarding, X11 forwarding, or file transfer support
- Intended as a small native `newos` client, not a full `ssh(1)` replacement

## EXAMPLES

```
ssh demo@example.com
ssh -p 2222 demo@127.0.0.1
ssh -i ~/.ssh/id_ed25519 git@github.com
ssh demo@127.0.0.1:2222 change-me
```

## SEE ALSO

man, netcat
