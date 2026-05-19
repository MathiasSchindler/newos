# WHICH

## NAME

which - locate a command in the PATH

## SYNOPSIS

```
which [-a] command ...
```

## DESCRIPTION

`which` searches the directories listed in `PATH` for each COMMAND and prints
the full path of the first (or all, with `-a`) matching executable.

## CURRENT CAPABILITIES

- Search PATH directories for an executable named COMMAND
- Print all matches, not just the first, with `-a`
- Recognise shell built-ins and report them as such
- Exit 0 if all commands were found, non-zero otherwise

## OPTIONS

- `-a` — print all matching paths, not just the first

## LIMITATIONS

- Shell aliases and functions are not resolved (only executables and built-ins are reported).
- `PATH` lookup only; no support for hash tables or per-shell caches.
- no `type`-style classification for keywords, reserved words, or shell
  functions
- executable checks depend on platform permission semantics and may differ on
  non-POSIX filesystems

## EXAMPLES

```
which ls
which -a python
which gcc make
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

find, sh
