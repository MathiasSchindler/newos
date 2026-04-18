# PS

## NAME

ps - list running processes

## SYNOPSIS

ps [-f] [-h] [-p PID[,PID...]] [-o FIELD[,FIELD...]]

## DESCRIPTION

The ps tool displays process information gathered from `/proc`. By default it prints the process ID, status, and command.

## CURRENT CAPABILITIES

- show a default `pid`, `stat`, and `command` view
- display a fuller process listing with `-f`
- filter to specific PIDs
- choose custom output fields from the supported set

## OPTIONS

| Flag | Description |
|------|-------------|
| `-f` | Use full format: `pid`, `ppid`, `user`, `stat`, `rss`, `command`. |
| `-h` | Suppress the header row. |
| `-p PID[,PID...]` | Show only the listed process IDs. |
| `-o FIELD[,FIELD...]` | Select output fields from `pid`, `ppid`, `user`, `stat`, `rss`, and `command`. |

## LIMITATIONS

- This implementation reads from `/proc` and is Linux-only.
- BSD-style flag syntax and user filtering with `-u` are not implemented.
- RSS values are reported in kilobytes.

## EXAMPLES

- `ps`
- `ps -f`
- `ps -p 1,123 -o pid,ppid,command`

## SEE ALSO

kill, top, uname
