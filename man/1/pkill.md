# PKILL

## NAME

pkill - signal processes by name

## SYNOPSIS

```
pkill [-SIGNAL] [-s SIGNAL|--signal SIGNAL] [-ix] [-u USER] [-P PPID] PATTERN
```

## DESCRIPTION

`pkill` scans the platform process table and sends a signal to processes whose command name matches PATTERN. It sends TERM by default.

## CURRENT CAPABILITIES

- regular-expression matching against process names
- exact-name matching with `-x`
- case-insensitive matching with `-i`
- filter by user or parent PID
- choose signals by name or number
- skips the running `pkill` process itself

## OPTIONS

- `-i` match case-insensitively
- `-x` require an exact process-name match
- `-u USER` match only processes owned by USER or numeric UID
- `-P PPID` match only children of PPID
- `-s SIGNAL` / `--signal SIGNAL` send SIGNAL instead of TERM
- `-SIGNAL` alternate short form for choosing the signal

## EXIT STATUS

- `0` at least one matching process was signaled
- `1` no process matched, or a signal delivery failed
- `2` usage error or process-table failure

## LIMITATIONS

- matching is based on the platform process-name field, not the full argument vector
- process visibility and signal permissions depend on the platform backend
- process groups, sessions, terminals, and newest/oldest selectors are not implemented

## EXAMPLES

```
pkill -HUP httpd
pkill -x sleep
pkill -TERM -u builduser worker
```

## SEE ALSO

pgrep, kill, ps, pstree
