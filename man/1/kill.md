# KILL

## NAME

kill - send signals to processes

## SYNOPSIS

kill [-l [SIGNAL]] [-s SIGNAL | --signal SIGNAL | -SIGNAL] [--] PID...

## DESCRIPTION

The kill tool sends signals to running processes. By default it sends `SIGTERM`, and it can also list or translate signal names and numbers.

## CURRENT CAPABILITIES

- send `SIGTERM` by default
- accept signal names or numbers
- list known signal names with `-l`
- translate signal names and numbers

## OPTIONS

| Flag | Description |
|------|-------------|
| `-l`, `--list` | List known signal names. |
| `-l SIGNAL` | Translate a signal number to a name, or a name to a number. |
| `-s SIGNAL` | Use the specified signal. |
| `--signal SIGNAL` | Use the specified signal. |
| `-SIGNAL` | Alternate short form for choosing the signal. |

## LIMITATIONS

- Process-group syntax with negative PIDs may not work on all platforms.
- The known signal-name list is fixed at build time.

## EXAMPLES

- `kill 1234`
- `kill -9 1234`
- `kill -l TERM`

## SEE ALSO

ps, sleep, sh
