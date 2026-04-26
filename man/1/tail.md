# TAIL

## NAME

tail - output the last part of files

## SYNOPSIS

```
tail [-n [+|-]COUNT | -c [+|-]COUNT] [-fFqv] [file ...]
```

## DESCRIPTION

The tail tool prints the end of files by line count or byte count and can optionally follow files as they grow.

## CURRENT CAPABILITIES

- print the last 10 lines by default
- select by lines or bytes
- start from a given line with `-n +N`
- follow appended data with `-f` or `-F`
- control multi-file headers with `-q` and `-v`

## OPTIONS

| Flag | Description |
|------|-------------|
| `-n [+\|-]COUNT` | Select lines from the end or start from line `+N`. |
| `-c [+\|-]COUNT` | Select bytes from the end or start from byte `+N`. |
| `-f` | Follow files for appended data. |
| `-F` | Follow by name and reopen if the file is replaced. |
| `-q` | Suppress file headers. |
| `-v` | Always print file headers. |

## LIMITATIONS

- Follow mode uses polling rather than inotify or kqueue.
- No `-s SLEEP_INTERVAL` flag is available.
- At most 32 files can be followed at once.

## EXAMPLES

```
tail file.txt
tail -n 50 server.log
tail -f app.log
```

## SEE ALSO

head, cat, wc
