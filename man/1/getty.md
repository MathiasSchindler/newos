# GETTY

## NAME

getty - attach a shell or login-style program to a tty path

## SYNOPSIS

```
getty [-nqi] [-r DELAY] [-l PROGRAM | -c COMMAND] [-t TERM]
      [-f ISSUE] [-p PROMPT] TTY [ARG ...]
```

## DESCRIPTION

`getty` opens a terminal device path, optionally prints a simple issue banner,
can prompt for a login name, and starts a shell or login-style program on that
tty. The tty path may also be `-` to reuse the current stdio streams in scripted
or test setups. If the child exits,
`getty` can restart it after a delay.

In this project it is designed as a small early-userspace helper, not as a full
traditional serial line manager.

## CURRENT CAPABILITIES

- run `/bin/sh` on a chosen tty path by default
- launch an explicit login program with `-l`
- run an inline shell command with `-c`
- pass additional arguments through to the launched program
- export `GETTY_TTY`, `TERM`, and optionally `GETTY_USER`/`USER`/`LOGNAME`
- sanitize the default `PATH` to `/bin:/usr/bin` and require absolute direct
  program paths for the launched session helper
- prompt for a simple login name with `-p`
- load a custom issue file with `-f`
- respawn the child after exit unless `-n` is used
- suppress banners and status chatter for quiet setups

## OPTIONS

- `-n`, `--no-respawn` - run the child once and exit with its status
- `-q`, `--quiet` - suppress the extra banner and restart notes
- `-i`, `--no-issue` - do not print `/etc/issue` before launching the session
- `-r DELAY`, `--restart-delay DELAY` - wait before respawning; accepts duration
  values such as `500ms`, `1s`, or `2m`
- `-l PROGRAM`, `--login PROGRAM` - set the program that should run on the tty
- `-c COMMAND`, `--command COMMAND` - run `sh -c COMMAND` on the tty instead of
  a positional program
- `-t TERM`, `--term TERM` - set the `TERM` value for the launched session
- `-f ISSUE`, `--issue-file ISSUE` - read banner text from a specific file
- `-p PROMPT`, `--prompt PROMPT` - prompt for a login name and export it to the
  launched program; when used with `-l`, the entered name is appended as an
  extra argument
- `-h`, `--help` - show usage information

## LIMITATIONS

- expects a tty-like device path; it does not probe serial settings or manage modem control lines
- no baud-rate parsing, `agetty`-style line discipline setup, or automatic serial configuration
- authentication is delegated entirely to the program you launch with `-l` or as the positional command; the built-in prompt only collects a user name
- respawn management is intentionally simple and only supervises one foreground child

## EXAMPLES

```
getty /dev/tty1
getty -n /dev/ttyS0 /bin/sh
getty -l /bin/login -p 'login: ' /dev/tty1
getty -c 'exec /bin/sh' -
getty -f /etc/issue.serial -t vt100 /dev/ttyS0
getty -q -r 2s /dev/console
```

## SEE ALSO

init, sh, man, hostname
