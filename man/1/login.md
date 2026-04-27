# LOGIN

## NAME

login - start a user session or command

## SYNOPSIS

```
login [-fp] [-h HOST] [-s SHELL] [USER [COMMAND...]]
```

## DESCRIPTION

`login` prepares a small user session environment and starts either the user's
shell or the supplied command. It looks up users through the platform identity
layer and reads `/etc/passwd` for home directory and shell fields when available.

This is an early-userspace session helper, not a complete account-security
implementation. Password verification is intentionally not implemented yet.
Without `-f`, `login` only permits logging in as the current uid. Trusted callers
such as `getty` or `init` may pass `-f` when they have already established that
the requested session should be started.

## OPTIONS

- `-f` - trusted login; allow switching to the requested account
- `-p` - preserve the current environment instead of creating a clean login
  environment
- `-h HOST` - record a remote host in `REMOTEHOST`
- `-s SHELL` - override the shell used when no command is supplied
- `--help` - show usage information

## ENVIRONMENT

Unless `-p` is used, `login` clears the environment and sets `HOME`, `SHELL`,
`USER`, `LOGNAME`, `PATH`, and optionally `REMOTEHOST`.

## LIMITATIONS

- Password verification, shadow-file handling, PAM, Kerberos, and multi-factor
  authentication are not implemented yet.
- Session accounting is minimal; it does not yet update a full utmp/wtmp-style
  login database for `who`, `users`, or `last`-style consumers.
- No account expiry, locked-account, login-class, resource-limit, or shell
  whitelist policy is enforced.
- Terminal ownership, permissions, and audit logging are intentionally simpler
  than a mature system `login(1)`.

## EXAMPLES

```
login
login mathias
login -f root /bin/sh
login -p "$USER" /bin/env
getty -l /bin/login -p 'login: ' /dev/tty1
```

## SEE ALSO

getty, init, sh, id, whoami
