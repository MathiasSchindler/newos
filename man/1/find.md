# FIND

## NAME

find - search for files in a directory hierarchy

## SYNOPSIS

```
find [path ...] [-name pattern] [-iname pattern] [-path pattern]
     [-type b|c|d|f|l|p|s] [-mtime n] [-newer file]
     [-size n[c|k|M]] [-perm mode] [-user user] [-group group] [-empty]
     [-mindepth n] [-maxdepth n]
     [! expr] [expr -a expr] [expr -o expr] [( expr )]
     [-prune] [-print|-print0] [-exec cmd {} ;]
```

## DESCRIPTION

`find` walks directory trees starting from each given path (default: `.`) and
prints paths that match the specified filters.

## CURRENT CAPABILITIES

- Filter by name glob pattern (`-name`)
- Filter by case-insensitive name glob pattern (`-iname`)
- Filter by full path glob pattern (`-path`)
- Filter by file type: `b`, `c`, `d`, `f`, `l`, `p`, or `s`
- Filter by modification time in days (`-mtime`)
- Filter by reference modification time with `-newer`
- Filter by file size with suffix `c` (bytes), `k` (kibibytes), `M` (mebibytes)
- Filter by permission bits, owner, or group with `-perm`, `-user`, and `-group`
- Match empty files and directories with `-empty`
- Limit recursion depth with `-mindepth` / `-maxdepth`
- Combine predicates with `!`, `-a`, `-o`, and parenthesized groups
- Prune matched directories from traversal with `-prune`
- Print results NUL-delimited with `-print0`
- Execute a command for each match with `-exec cmd {} ;`

## OPTIONS

- `-name PATTERN` — match basename against shell glob PATTERN
- `-iname PATTERN` — case-insensitive basename match against shell glob PATTERN
- `-path PATTERN` — match full path against shell glob PATTERN
- `-type b|c|d|f|l|p|s` — restrict to block devices, character devices, directories, regular files, symlinks, FIFOs, or sockets
- `-mtime N` — match files modified N days ago (`+N` more than, `-N` less than)
- `-newer FILE` — match files whose modification time is newer than FILE
- `-size N[c|k|M]` — match by file size; default unit is bytes
- `-perm MODE` — match exact permission bits; use `-MODE` for all bits or `/MODE` for any bit
- `-user USER` — match a numeric UID or user name
- `-group GROUP` — match a numeric GID or group name
- `-empty` — match zero-length files and empty directories
- `-mindepth N` — skip entries less than N levels deep
- `-maxdepth N` — do not descend more than N levels
- `! EXPR`, `-not EXPR` — negate a predicate or grouped expression
- `EXPR -a EXPR`, `EXPR -and EXPR` — require both sides to match
- `EXPR -o EXPR`, `EXPR -or EXPR` — require either side to match
- `( EXPR )` — group predicates to control precedence
- `-prune` — do not recurse into matched directories
- `-print` — print matching path (default action)
- `-print0` — print matching path followed by NUL byte
- `-exec CMD {} ;` — execute CMD with `{}` replaced by the path

## LIMITATIONS

- `-exec ... +` is accepted for compatibility but currently executes once per match like `;`.
- Symbolic `-perm` modes such as `u+r` are not implemented; use octal modes.

## EXAMPLES

```
find . -name "*.c"
find . \( -name .git -o -name build \) -prune -o -type f -print
find src -type f -name "*.h"
find . -mtime -7 -print
find . -newer stamp.file -type f -print
find . -perm -600 -user 1000 -print
find . -maxdepth 2 -type d
find . -name "*.o" -exec rm {} ;
```

## SEE ALSO

grep, ls, stat
