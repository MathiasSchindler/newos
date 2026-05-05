# PGREP

## NAME

pgrep - find processes by name

## SYNOPSIS

```
pgrep [-ilcx] [-u USER] [-P PPID] PATTERN
```

## DESCRIPTION

`pgrep` scans the platform process table and prints process IDs whose command name matches PATTERN.

## CURRENT CAPABILITIES

- regular-expression matching against process names
- exact-name matching with `-x`
- case-insensitive matching with `-i`
- print matching names with `-l`
- count matches with `-c`
- filter by user or parent PID

## OPTIONS

- `-i` match case-insensitively
- `-l` print the process name after each PID
- `-c` print only the number of matches
- `-x` require an exact process-name match
- `-u USER` match only processes owned by USER or numeric UID
- `-P PPID` match only children of PPID

## EXIT STATUS

- `0` one or more processes matched
- `1` no processes matched
- `2` usage error or process-table failure

## LIMITATIONS

- matching is based on the platform process-name field, not the full argument vector
- regular expression behavior follows the project shared regex helper
- process visibility depends on the platform backend and permissions

## EXAMPLES

```
pgrep sh
pgrep -l -u root sshd
pgrep -x -P 1 getty
```

## SEE ALSO

pkill, ps, kill, pstree
