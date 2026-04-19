# MAN

## NAME

man - view project-local manual pages stored in the repository

## SYNOPSIS

```
man [--color[=WHEN]] [SECTION] TOPIC
man [--color[=WHEN]] -k KEYWORD
man [--color[=WHEN]] -l FILE
```

## DESCRIPTION

The project `man` tool reads Markdown manual pages from repository-local
`man/` directories and from any paths supplied through `MANPATH`. It does not
depend on the host system manual database and does not install files into
system locations.

Pages are searched across whatever numbered sections exist in the manual tree;
in this repository that is primarily section 1 (tools) and section 7
(components and project notes).

When writing to an interactive terminal, `man` also provides a lightweight
built-in pager prompt and renders common Markdown structures into a more
readable terminal layout.

## CURRENT CAPABILITIES

- look up a page by topic name, optionally prefixed with a section number
- case-insensitive keyword search across page names and page content
- display an arbitrary Markdown file directly with `-l`
- search repository-local manuals without requiring roff or a system database
- render headings, quotes, fenced code blocks, and Markdown tables in a cleaner
  terminal form
- display tables with aligned ASCII borders instead of flattened pipe-separated rows
- pause between terminal-sized screens in interactive use
- use shared terminal colors with `--color=WHEN`

## OPTIONS

- `SECTION` — selects a specific manual section (e.g. 1, 5, 7)
- `-k KEYWORD` — searches page names and content for the keyword
- `-l FILE` — displays the specified Markdown file directly
- `--color[=WHEN]` — control terminal styling with `auto`, `always`, or `never`
- `--help` — print the usage summary

## ENVIRONMENT

- `MANPATH` — additional manual roots to search, separated by `:`
- `LINES` — preferred interactive page height
- `COLUMNS` — preferred output width for wrapping and table layout
- `NO_COLOR`, `CLICOLOR`, `CLICOLOR_FORCE`, `TERM` — influence shared color behavior

## LIMITATIONS

- only covers pages found in the repository `man/` tree or `MANPATH`; it does
  not consult system-installed manuals
- the source format is Markdown, not traditional roff macros
- table rendering is intentionally simple ASCII framing; complex spanning/layout
  features are not implemented
- no roff macro compatibility, external pager handoff, hyperlink activation, or
  rich terminal widgets

## EXAMPLES

```
man ls
man 1 cp
man -k compiler
MANPATH=extras/man:man man topic
man -l man/1/ncc.md
man --color=always ls
```

## SEE ALSO

less, ls, cp, ncc, output-style
