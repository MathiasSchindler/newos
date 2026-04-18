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

The project `man` tool reads Markdown manual pages from the repository-local
`man/` directory. It does not depend on the host system manual database and
does not install files into system locations.

Pages are searched in sections such as 1, 5, and 7.

## CURRENT CAPABILITIES

- Look up a page by topic name, optionally prefixed with a section number
- Keyword search across page names and page content
- Display an arbitrary Markdown file directly with `-l`

## OPTIONS

- `SECTION` — selects a specific manual section (e.g. 1, 5, 7)
- `-k KEYWORD` — searches page names and content for the keyword
- `-l FILE` — displays the specified Markdown file directly

## LIMITATIONS

- Only covers pages present in the repository `man/` tree; no system pages.
- No terminal paging; output is written directly to stdout.

## EXAMPLES

```
man ls
man 1 cp
man -k compiler
man -l man/1/ncc.md
```

## SEE ALSO

ls, cp, ncc
