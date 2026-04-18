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

- Shell aliases and functions are not resolved (only executables and
  built-ins are reported).
- `PATH` lookup only; no support for hash tables or per-shell caches.

## EXAMPLES

```
which ls
which -a python
which gcc make
```

## SEE ALSO

find, sh
