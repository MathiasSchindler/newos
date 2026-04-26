# BASENAME

## NAME

basename - strip directory components and optional suffix from a path

## SYNOPSIS

```
basename [-a] [-s SUFFIX] [-z] name ...
```

## DESCRIPTION

`basename` prints the final component of each PATH, optionally removing a
trailing SUFFIX. With `-a`, multiple names are accepted and processed in order.

## CURRENT CAPABILITIES

- Strip leading directory components from one or more paths
- Remove a trailing suffix string with `-s`
- Separate output entries with NUL instead of newline with `-z`
- Process multiple arguments with `-a`

## OPTIONS

- `-a`, `--multiple` — allow multiple NAME arguments
- `-s SUFFIX`, `--suffix=SUFFIX` — remove a trailing SUFFIX from each name
- `-z`, `--zero` — end each output line with NUL rather than newline

## LIMITATIONS

- Without `-a`, only a single NAME argument is accepted; a suffix may follow as a second positional argument (classic POSIX form: `basename name suffix`).
- No `-n` (no newline) option.

## EXAMPLES

```
basename /usr/lib/libc.so.6
basename /path/to/file.txt .txt
basename -a -s .c src/foo.c src/bar.c
basename -z /a/b/c
```

## SEE ALSO

dirname, realpath
