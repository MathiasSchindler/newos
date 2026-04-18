# DIRNAME

## NAME

dirname - strip the last component from a path

## SYNOPSIS

```
dirname [-z] path ...
```

## DESCRIPTION

`dirname` prints the directory component of each PATH — everything up to and
including the last `/`. If no `/` is present, `.` is printed.

## CURRENT CAPABILITIES

- Strip the last component from one or more paths
- Separate output entries with NUL instead of newline with `-z`

## OPTIONS

- `-z`, `--zero` — end each output line with NUL rather than newline

## LIMITATIONS

- No `-n` (suppress trailing newline) option.

## EXAMPLES

```
dirname /usr/lib/libc.so.6
dirname /path/to/file.txt
dirname file.txt
dirname -z /a/b /c/d
```

## SEE ALSO

basename, realpath
