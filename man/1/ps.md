# PS

## NAME

ps - list running processes

## SYNOPSIS

```
ps [-f] [-e|-A|-a|-x] [aux] [-h|--no-headers]
   [-p PID[,PID...]] [-u USER[,USER...]] [-s STATE[,STATE...]]
   [-o FIELD[,FIELD...]] [--sort FIELD] [-r]
```

## DESCRIPTION

The ps tool displays process information gathered from the platform process
layer. The default output includes `pid`, `ppid`, `user`, `stat`, `rss`, and
`command`, and the command also accepts several familiar compatibility forms.

## CURRENT CAPABILITIES

- show a default full-format process table
- accept common `-f`, `-e`, `-A`, `-a`, `-x`, and bare BSD-style `aux` forms
- sort the displayed process list by PID
- filter to specific PIDs
- filter by owning user or process state
- choose custom output fields from the supported set and rename headers with
  `field=HEADER`
- sort by `pid`, `ppid`, `uid`, `user`, `stat`, `rss`, or `command`

## OPTIONS

| Flag | Description |
|------|-------------|
| `-f` | Accepted for compatibility; the default output is already full format. |
| `-e`, `-A`, `-a`, `-x` | Accepted compatibility forms for showing the normal full listing. |
| `aux` | BSD-style compatibility form for the normal full listing. |
| `-h`, `--no-headers` | Suppress the header row. |
| `-p PID[,PID...]` | Show only the listed process IDs. |
| `-u USER[,USER...]` | Show only processes owned by the listed users or numeric UIDs. |
| `-s STATE[,STATE...]` | Filter by process state prefix such as `R`, `S`, or `Z`. |
| `-o FIELD[,FIELD...]` | Select output fields from `pid`, `ppid`, `uid`, `user`, `stat`/`state`, `rss`/`rss_kb`, and `command`/`cmd`/`comm`. Use `field=HEADER` to rename or blank a header. |
| `--sort FIELD` | Sort by a supported field; prefix with `-` for descending order. |
| `-r`, `--reverse` | Reverse the selected sort order. |

## LIMITATIONS

- The available columns are intentionally compact; terminal, elapsed time, and full argument-vector display are not yet exposed through the shared platform interface.
- RSS values are reported in kilobytes.

## EXAMPLES

```
ps
ps aux
ps --no-headers -p 1
ps -u root -o pid,uid,user,command
ps --sort=-rss -o pid,user,rss,command
```

## SEE ALSO

kill, top, uname
