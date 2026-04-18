# REALPATH

## NAME

realpath - resolve a path to its canonical absolute form

## SYNOPSIS

```
realpath [-e|-m] [-L|-P] [-q] path ...
```

## DESCRIPTION

`realpath` resolves each PATH by expanding all symbolic links and removing
`.` and `..` components, then prints the resulting absolute path.

## CURRENT CAPABILITIES

- Resolve symlinks and normalize path components
- Require all components to exist with `-e`
- Allow missing final component with `-m`
- Logical (no symlink resolution) mode with `-L`
- Physical (resolve all symlinks) mode with `-P` (default)
- Suppress error messages with `-q`

## OPTIONS

- `-e`, `--canonicalize-existing` — all path components must exist
- `-m`, `--canonicalize-missing` — allow the final component to be absent
- `-L`, `--logical` — resolve without following symlinks
- `-P`, `--physical` — resolve following all symlinks (default)
- `-q`, `--quiet` — suppress error messages on non-existent paths

## LIMITATIONS

- No `-s` (strip; do not resolve symlinks at all).
- No `--relative-to` or `--relative-base` options.
- No NUL-separated output mode.

## EXAMPLES

```
realpath ./some/../path
realpath -e /proc/self
realpath -m /nonexistent/path
realpath -q missing
```

## SEE ALSO

basename, dirname, readlink
