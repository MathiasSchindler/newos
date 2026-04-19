# GETTY

## NAME

getty - attach a shell or login-style program to a tty path

## SYNOPSIS

```sh
getty [-nqi] [-r DELAY] [-l PROGRAM] TTY [ARG ...]
```

## DESCRIPTION

`getty` opens a terminal device path, optionally prints a simple issue banner,
and starts a shell or login-style program on that tty. If the child exits,
`getty` can restart it after a delay.

In this project it is designed as a small early-userspace helper, not as a full
traditional serial line manager.

## CURRENT CAPABILITIES

- run `/bin/sh` on a chosen tty path by default
- launch an explicit login program with `-l`
- pass additional arguments through to the launched program
- respawn the child after exit unless `-n` is used
- suppress banners and status chatter for quiet setups

## OPTIONS

- `-n`, `--no-respawn` - run the child once and exit with its status
- `-q`, `--quiet` - suppress the extra banner and restart notes
- `-i`, `--no-issue` - do not print `/etc/issue` before launching the session
- `-r DELAY`, `--restart-delay DELAY` - wait before respawning; accepts duration
  values such as `500ms`, `1s`, or `2m`
- `-l PROGRAM`, `--login PROGRAM` - set the program that should run on the tty
- `-h`, `--help` - show usage information

## LIMITATIONS

- expects a tty-like device path; it does not probe serial settings or manage
  modem control lines
- no baud-rate parsing, `agetty`-style line discipline setup, or automatic
  login-name prompt yet
- authentication is delegated entirely to the program you launch with `-l` or as
  the positional command
- respawn management is intentionally simple and only supervises one foreground
  child

## EXAMPLES

```sh
getty /dev/tty1
getty -n /dev/ttyS0 /bin/sh
getty -l /bin/login /dev/tty1
getty -q -r 2s /dev/console
```

## SEE ALSO

init, sh, man, hostname
