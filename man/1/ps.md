# PS

## NAME

ps - list running processes

## SYNOPSIS

ps [-f] [-e|-A|-a|-x] [-h|--no-headers] [-p PID[,PID...]] [-o FIELD[,FIELD...]]

## DESCRIPTION

The ps tool displays process information gathered from `/proc`. The current
default output already includes `pid`, `ppid`, `user`, `stat`, `rss`, and
`command`.

## CURRENT CAPABILITIES

- show a default full-format process table
- accept common `-f`, `-e`, `-A`, `-a`, and `-x` compatibility flags
- sort the displayed process list by PID
- filter to specific PIDs
- choose custom output fields from the supported set

## OPTIONS

| Flag | Description |
|------|-------------|
| `-f` | Accepted for compatibility; the default output is already full format. |
| `-e`, `-A`, `-a`, `-x` | Accepted compatibility forms for showing the normal full listing. |
| `-h`, `--no-headers` | Suppress the header row. |
| `-p PID[,PID...]` | Show only the listed process IDs. |
| `-o FIELD[,FIELD...]` | Select output fields from `pid`, `ppid`, `user`, `stat`/`state`, `rss`/`rss_kb`, and `command`/`cmd`/`comm`. |

## LIMITATIONS

- This implementation reads from `/proc` and is Linux-only.
- user filtering with `-u` and BSD-style bare forms such as `aux` are not implemented.
- compatibility flags such as `-f`, `-e`, and `-x` are accepted, but they do
  not currently change filtering beyond the normal full listing
- RSS values are reported in kilobytes.

## EXAMPLES

- `ps`
- `ps -f`
- `ps --no-headers -p 1`
- `ps -p 1,123 -o pid,ppid,command`

## SEE ALSO

kill, top, uname
