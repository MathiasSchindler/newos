# LESS

## NAME

less - page through text one screen at a time

## SYNOPSIS

```
less [-N] [-p PATTERN] [--color[=WHEN]] [+/PATTERN] [+LINE] [+PERCENT%] [file ...]
```

## DESCRIPTION

less displays text files (or standard input) one screen at a time, pausing for
input after each page. It keeps a compact in-memory view of the current file so
that common pager navigation works without external dependencies.

## CURRENT CAPABILITIES

- forward and backward paging through files
- optional line numbering
- initial search-jump with `-p PATTERN` or `+/PATTERN`
- initial line and percentage jumps with `+LINE` and `+PERCENT%`
- repeated next-match search with `n` after an interactive `/pattern`
- page and line navigation with `Space`, `Enter`, `j`, `k`, `b`, `g`, and `G`
- reading from standard input when no files are given
- reading from multiple files in sequence
- lightweight ANSI styling for prompts and match emphasis with `--color=WHEN`

## OPTIONS

- `-N` — prefix each line with its line number
- `-p PATTERN` or `+/PATTERN` — start near the first matching line
- `+LINE` — start at the requested 1-based line number
- `+PERCENT%` — start near the requested percentage of the input
- `--color[=WHEN]` — control prompt and match styling with `auto`, `always`,
  or `never`

## LIMITATIONS

- interactive search and navigation are intentionally compact rather than a full clone of GNU less
- no syntax highlighting or arbitrary terminal-widget support
- noninteractive unsearched input streams directly; searched or jumped input is
  buffered and remains bounded by the in-memory pager index
- no mark/register commands, horizontal scrolling modes, follow mode, or
  external editor integration are implemented
- terminal control uses the shared built-in ANSI capability profile rather than terminfo; arrow keys, page keys, and resize-driven page height are handled through the shared streaming input decoder

## EXAMPLES

- `less file.txt` — page through a file
- `less -N file.txt` — page through a file with line numbers
- `less -p error build.log` — open near the first matching line
- `less +120 build.log` — open near line 120
- `less +50% build.log` — open near the middle of the file
- `cat long.log | less` — page through piped output

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

more, man, cat, head, tail
