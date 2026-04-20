# LESS

## NAME

less - page through text one screen at a time

## SYNOPSIS

less [-N] [-p PATTERN] [--color[=WHEN]] [+/PATTERN] [file ...]

## DESCRIPTION

less displays text files (or standard input) one screen at a time, pausing for
input after each page. It keeps a compact in-memory view of the current file so
that common pager navigation works without external dependencies.

## CURRENT CAPABILITIES

- forward and backward paging through files
- optional line numbering
- initial search-jump with `-p PATTERN` or `+/PATTERN`
- repeated next-match search with `n` after an interactive `/pattern`
- page and line navigation with `Space`, `Enter`, `j`, `k`, `b`, `g`, and `G`
- reading from standard input when no files are given
- reading from multiple files in sequence
- lightweight ANSI styling for prompts and match emphasis with `--color=WHEN`

## OPTIONS

- `-N` — prefix each line with its line number
- `-p PATTERN` or `+/PATTERN` — start near the first matching line
- `--color[=WHEN]` — control prompt and match styling with `auto`, `always`,
  or `never`

## LIMITATIONS

- interactive search and navigation are intentionally compact rather than a
  full clone of GNU less
- no syntax highlighting or arbitrary terminal-widget support
- very large inputs may fall back to simpler streaming behavior

## EXAMPLES

- `less file.txt` — page through a file
- `less -N file.txt` — page through a file with line numbers
- `less -p error build.log` — open near the first matching line
- `cat long.log | less` — page through piped output

## SEE ALSO

more, man, cat, head, tail
