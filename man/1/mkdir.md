# MKDIR

## NAME

mkdir - create directories

## SYNOPSIS

```
mkdir [-p] [-v] [-m mode] directory ...
```

## DESCRIPTION

`mkdir` creates one or more directories and can optionally create missing
parents and apply an explicit octal mode.

## CURRENT CAPABILITIES

- create one or more directories
- create parent directories with `-p`
- quietly accept already-existing parents in `-p` mode
- verbose creation messages with `-v`
- set an explicit mode with `-m`

## OPTIONS

- `-p` create parent directories as needed and do not fail if they already exist
- `-v` print a line for each directory actually created
- `-m MODE` apply an explicit permission mode (typically octal such as `755`)

## LIMITATIONS

- symbolic mode strings are not guaranteed; `-m` is intended for numeric/octal modes
- when `-m` is not supplied, final permissions still depend on the current process umask
- SELinux context options, verbose parent-by-parent reporting quirks, and
  GNU-specific long options beyond the documented set are not implemented.
- `-p` handles ordinary parent creation but does not provide transactional
  rollback if a later component fails.

## EXAMPLES

```
mkdir work
mkdir -p build/tmp/cache
mkdir -m 700 private-dir
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

rmdir, rm, chmod
