# SSHD

`sshd` is a minimal newos-native SSH server.

## SYNOPSIS

```
sshd [-v] [-1] [-p PORT] [-l ADDRESS] [-u USER]
  -P PASSWORD|@file|@@literal|- [-k HOSTKEY_SEED_FILE] [-s SHELL]
```

## OPTIONS

- `--help`, `-h`: show help.
- `-v`: print basic connection diagnostics.
- `-1`: handle one client and exit. This is useful for tests.
- `-p PORT`: listen port. The default is `2222`.
- `-l ADDRESS`: listen address. The default is `0.0.0.0`.
- `-u USER`: user name accepted by password authentication.
- `-P PASSWORD|@file|@@literal|-`: configure the accepted password. `@file`
  reads a secure file, `@@literal` keeps a leading `@`, and `-` reads one
  line from standard input.
- `-k HOSTKEY_SEED_FILE`: read a persistent Ed25519 host-key seed from a
  secure file. The file must contain 32 raw bytes or 64 hex characters. Without
  this option, `sshd` generates an ephemeral host key.
- `-s SHELL`: command runner used for exec requests as `SHELL -c COMMAND`.
  The default is `sh`.

## CURRENT SCOPE

This first milestone implements SSH-2.0 transport using
`curve25519-sha256`, `ssh-ed25519`, and
`chacha20-poly1305@openssh.com`, password authentication, one session channel,
and bounded `exec` requests. It does not implement a generic PTY API, so
`pty-req` and interactive `shell` requests are rejected with channel failure.

This tool is experimental and does not provide privilege separation, sandboxing,
account lookup, public-key authentication, or rekeying.
