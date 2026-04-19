# MAN

## NAME

man - view project-local manual pages stored in the repository

## SYNOPSIS

```
man [SECTION] TOPIC
man -k KEYWORD
man -l FILE
```

## DESCRIPTION

The project `man` tool reads Markdown manual pages from repository-local
`man/` directories and from any paths supplied through `MANPATH`. It does not
depend on the host system manual database and does not install files into
system locations.

Pages are searched in sections such as 1, 5, and 7.

## CURRENT CAPABILITIES

- look up a page by topic name, optionally prefixed with a section number
- case-insensitive keyword search across page names and page content
- display an arbitrary Markdown file directly with `-l`
- search repository-local manuals without requiring roff or a system database

## OPTIONS

- `SECTION` — selects a specific manual section (e.g. 1, 5, 7)
- `-k KEYWORD` — searches page names and content for the keyword
- `-l FILE` — displays the specified Markdown file directly

## LIMITATIONS

- only covers pages found in the repository `man/` tree or `MANPATH`; it does
  not consult system-installed manuals
- output is written directly to stdout; there is no built-in pager stage
- the source format is Markdown, not traditional roff macros

## EXAMPLES

```
man ls
man 1 cp
man -k compiler
MANPATH=extras/man:man man topic
man -l man/1/ncc.md
```

## SEE ALSO

ls, cp, ncc
