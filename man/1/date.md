# DATE

## NAME

date - print or convert the current date and time

## SYNOPSIS

```
date [-u|-l] [-d TEXT|-r FILE] [+FORMAT]
```

## DESCRIPTION

`date` prints the current date and time formatted according to FORMAT. Without
a format argument it uses a default locale-style representation. The `-d` and
`-r` options allow formatting an arbitrary date string or a file's modification
time instead of the current time.

## CURRENT CAPABILITIES

- Print current time in UTC with `-u` or local time with `-l`
- Format output using `strftime`-style `+FORMAT` string
- Parse a date text with `-d TEXT` (or `--date=TEXT`)
- Use a file's modification time as the source with `-r FILE` (or
  `--reference=FILE`)

## OPTIONS

- `-u` — use UTC
- `-l` — use local time (default)
- `-d TEXT`, `--date=TEXT` — display time described by TEXT instead of now
- `-r FILE`, `--reference=FILE` — display last modification time of FILE
- `+FORMAT` — format string; all `strftime` specifiers are supported

## LIMITATIONS

- The `-s` (set system time) option is not implemented.
- Date arithmetic in `-d` expressions may be limited to what the platform `strptime` supports.
- No `--iso-8601` or `--rfc-3339` convenience flags.

## EXAMPLES

```
date
date +"%Y-%m-%d"
date -u +"%H:%M:%S"
date -d "2024-01-01" +"%A"
date -r file.txt +"%s"
```

## SEE ALSO

stat
