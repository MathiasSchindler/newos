# KILL

## NAME

kill - send signals to processes

## SYNOPSIS

kill [-l [SIGNAL]] [-L] [-s SIGNAL | --signal SIGNAL | -SIGNAL] [--] PID...

## DESCRIPTION

The kill tool sends signals to running processes. By default it sends
`SIGTERM`, and it can also list or translate signal names and numbers.

## CURRENT CAPABILITIES

- send `SIGTERM` by default to one or more PIDs
- accept signal names or numbers
- list known signal names with `-l`
- translate signal names and numbers

## OPTIONS

| Flag | Description |
|------|-------------|
| `-l`, `-L`, `--list` | List known signal names. |
| `-l SIGNAL` | Translate a signal number to a name, or a name to a number. |
| `--list=SIGNAL` | Translate a signal name or number without sending anything. |
| `-s SIGNAL` | Use the specified signal. |
| `--signal SIGNAL` | Use the specified signal. |
| `-SIGNAL` | Alternate short form for choosing the signal. |
| `--` | End option parsing and treat remaining arguments as PIDs. |

## LIMITATIONS

- Process-group syntax with negative PIDs may not work on all platforms.
- The known signal-name list is fixed at build time.
- No process-name matching or pattern-based selection is provided.
- When several PIDs are given, `kill` continues attempting later ones even if an
  earlier signal delivery fails.

## EXAMPLES

- `kill 1234`
- `kill --signal TERM 1234 1235`
- `kill -9 1234`
- `kill -l TERM`

## SEE ALSO

ps, sleep, sh
