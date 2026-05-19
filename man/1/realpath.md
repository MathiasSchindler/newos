# REALPATH

## NAME

realpath - resolve a path to its canonical absolute form

## SYNOPSIS

```
realpath [-e|-m] [-L|-P] [-q] [-z] path ...
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
- NUL-separated output with `-z`

## OPTIONS

- `-e`, `--canonicalize-existing` — all path components must exist
- `-m`, `--canonicalize-missing` — allow the final component to be absent
- `-L`, `--logical` — resolve without following symlinks
- `-P`, `--physical` — resolve following all symlinks (default)
- `-q`, `--quiet` — suppress error messages on non-existent paths
- `-z`, `--zero` — terminate each resolved path with NUL instead of newline

## LIMITATIONS

- No `-s` (strip; do not resolve symlinks at all).
- No `--relative-to` or `--relative-base` options.

## EXAMPLES

```
realpath ./some/../path
realpath -e /proc/self
realpath -m /nonexistent/path
realpath -z file1 file2
realpath -q missing
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

basename, dirname, readlink
