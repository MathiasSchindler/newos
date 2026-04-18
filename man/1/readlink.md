# READLINK

## NAME

readlink - print the value of a symbolic link or canonical path

## SYNOPSIS

```
readlink [-n] [-f|-e|-m] [-q] [-v] [-z] PATH ...
```

## DESCRIPTION

`readlink` prints the target of each symbolic link PATH. With canonicalisation
flags (`-f`, `-e`, `-m`) it resolves the full canonical path similarly to
`realpath`.

## CURRENT CAPABILITIES

- Print the raw symlink target for a symbolic link
- Canonicalise the path (resolve all symlinks) with `-f`
- Require path to exist with `-e`; allow missing final component with `-m`
- Suppress trailing newline with `-n`
- Suppress error messages with `-q`/`-s`
- Verbose error messages with `-v`
- NUL-separated output with `-z`

## OPTIONS

- `-n` — suppress trailing newline
- `-f`, `--canonicalize` — resolve the full canonical path (symlinks may
  not exist)
- `-e`, `--canonicalize-existing` — like `-f` but all components must exist
- `-m`, `--canonicalize-missing` — like `-f` but the final component may
  be absent
- `-q`, `-s`, `--silent`, `--quiet` — suppress error messages
- `-v`, `--verbose` — report errors
- `-z`, `--zero` — separate output items with NUL

## LIMITATIONS

- Without a canonicalisation flag, returns only the direct symlink target
  (one level); it does not recursively resolve chains.
- No `--no-newline` long-form alias for `-n` in this implementation.

## EXAMPLES

```
readlink /proc/self/exe
readlink -f ./relative/../path
readlink -e /etc/localtime
readlink -q /not/a/symlink || echo "not a symlink"
```

## SEE ALSO

realpath, stat, ln
