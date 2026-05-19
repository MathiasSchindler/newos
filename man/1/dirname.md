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
- No long-option compatibility beyond `--zero`.
- Path handling is lexical; it does not resolve symlinks, normalize `..`, or
  consult the filesystem. Use `realpath` for canonical paths.

## EXAMPLES

```
dirname /usr/lib/libc.so.6
dirname /path/to/file.txt
dirname file.txt
dirname -z /a/b /c/d
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

basename, realpath
