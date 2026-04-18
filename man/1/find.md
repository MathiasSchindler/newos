# FIND

## NAME

find - search for files in a directory hierarchy

## SYNOPSIS

```
find [path ...] [-name pattern] [-path pattern] [-type f|d|l]
     [-mtime n] [-size n[c|k|M]] [-mindepth n] [-maxdepth n]
     [-prune] [-print|-print0] [-exec cmd {} ;]
```

## DESCRIPTION

`find` walks directory trees starting from each given path (default: `.`) and
prints paths that match the specified filters.

## CURRENT CAPABILITIES

- Filter by name glob pattern (`-name`)
- Filter by full path glob pattern (`-path`)
- Filter by file type: `f` (regular file), `d` (directory), `l` (symlink)
- Filter by modification time in days (`-mtime`)
- Filter by file size with suffix `c` (bytes), `k` (kibibytes), `M` (mebibytes)
- Limit recursion depth with `-mindepth` / `-maxdepth`
- Prune matched directories from traversal with `-prune`
- Print results NUL-delimited with `-print0`
- Execute a command for each match with `-exec cmd {} ;`

## OPTIONS

- `-name PATTERN` — match basename against shell glob PATTERN
- `-path PATTERN` — match full path against shell glob PATTERN
- `-type f|d|l` — restrict to regular files, directories, or symlinks
- `-mtime N` — match files modified N days ago (`+N` more than, `-N` less than)
- `-size N[c|k|M]` — match by file size; default unit is bytes
- `-mindepth N` — skip entries less than N levels deep
- `-maxdepth N` — do not descend more than N levels
- `-prune` — do not recurse into matched directories
- `-print` — print matching path (default action)
- `-print0` — print matching path followed by NUL byte
- `-exec CMD {} ;` — execute CMD with `{}` replaced by the path

## LIMITATIONS

- Only `f`, `d`, and `l` type filters are supported; `b`, `c`, `p`, `s` are not.
- `-exec ... +` (batch form) is parsed but may behave like `;`.
- No `-newer`, `-perm`, `-user`, `-group`, or logical operators (`-o`, `-a`, `!`).

## EXAMPLES

```
find . -name "*.c"
find src -type f -name "*.h"
find . -mtime -7 -print
find . -maxdepth 2 -type d
find . -name "*.o" -exec rm {} ;
```

## SEE ALSO

grep, ls, stat
