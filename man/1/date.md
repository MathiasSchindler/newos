# DATE

## NAME

date - print or convert the current date and time

## SYNOPSIS

```
date [-u|-l] [-d TEXT|-r FILE] [--iso-8601[=PRECISION]|--rfc-3339=PRECISION] [+FORMAT]
```

## DESCRIPTION

`date` prints the current date and time formatted according to FORMAT. Without
a format argument it uses a default locale-style representation. The `-d` and
`-r` options allow formatting an arbitrary date string or a file's modification
time instead of the current time.

## CURRENT CAPABILITIES

- Print current time in UTC with `-u` or local time with `-l`
- Format output using `strftime`-style `+FORMAT` string
- Parse a date text with `-d TEXT` (or `--date=TEXT`), including ISO-like
  timestamps, timezone suffixes, and relative arithmetic
- Use a file's modification time as the source with `-r FILE` (or
  `--reference=FILE`)
- Emit ISO 8601 and RFC 3339 convenience formats

## OPTIONS

- `-u` — use UTC
- `-l` — use local time
- `-d TEXT`, `--date=TEXT` — display time described by TEXT instead of now
- `-r FILE`, `--reference=FILE` — display last modification time of FILE
- `--iso-8601[=PRECISION]`, `-I[PRECISION]` — use ISO 8601 output; PRECISION may be `date`, `hours`, `minutes`, `seconds`, or `ns`
- `--rfc-3339=PRECISION` — use RFC 3339-style output; PRECISION may be `date`, `seconds`, or `ns`
- `+FORMAT` — format string; all `strftime` specifiers are supported

## LIMITATIONS

- The `-s` (set system time) option is rejected with a clear error because the
  platform layer does not expose system clock mutation.

## EXAMPLES

```
date
date +"%Y-%m-%d"
date -u +"%H:%M:%S"
date -d "2024-01-01" +"%A"
date -d "2024-01-01 + 2 weeks" +"%Y-%m-%d"
date -r file.txt +"%s"
date --iso-8601=seconds
date --rfc-3339=seconds -d @0
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

stat
