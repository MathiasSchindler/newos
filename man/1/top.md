# TOP

## NAME

top - display a live-style summary of the busiest processes

## SYNOPSIS

```
top [-b] [-n ROWS] [-p PID[,PID...]] [-o FIELD] [-r]
```

## DESCRIPTION

top shows a compact system summary followed by a process table ordered by the
selected sort key. The implementation is intentionally lightweight and follows
the same platform process and memory interfaces used by ps, free, and uptime.

The current command focuses on a single snapshot rather than a full-screen
interactive dashboard, which keeps it portable across the hosted and
freestanding-oriented builds in this project.

## CURRENT CAPABILITIES

- summary banner with current time, uptime, task count, and load average
- task-state summary for running, sleeping, stopped, and zombie processes
- memory summary using the shared platform memory interface
- process table with PID, user, state, RSS, and command name
- default ordering by resident memory use
- optional PID filtering for focused inspection
- alternate sorting for `pid`, `ppid`, `uid`, `user`, `state`, `rss`, and `command`

## OPTIONS

| Flag | Description |
|------|-------------|
| `-b`, `--batch` | Accepted for compatibility. The current implementation already prints a non-interactive snapshot. |
| `-n ROWS`, `--lines ROWS` | Limit the displayed process rows. The default is `20`. |
| `-p PID[,PID...]`, `--pid PID[,PID...]` | Show only the listed process IDs. |
| `-o FIELD`, `--sort FIELD` | Sort by `pid`, `ppid`, `uid`, `user`, `state`, `rss`, or `command`. |
| `-r`, `--reverse` | Reverse the chosen sort order. |

## LIMITATIONS

- no full-screen interactive refresh mode yet
- CPU percentage and per-thread statistics are not displayed yet
- the command shows the process name rather than the full argument vector
- sorting is single-key only

## EXAMPLES

- `top` — show a default memory-sorted snapshot
- `top -n 10` — show the first ten rows
- `top -p 1,$$` — inspect a few selected processes
- `top -o pid -r` — list processes in descending PID order

## SEE ALSO

ps, pstree, free, uptime
