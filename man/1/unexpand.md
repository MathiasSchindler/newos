# UNEXPAND

## NAME

unexpand - convert spaces to tabs

## SYNOPSIS

```
unexpand [-aiz] [-t TABSTOP[,TABSTOP...]] [file ...]
```

## DESCRIPTION

unexpand reads files (or standard input) and converts runs of spaces back to tab characters where they align with tab stops. By default only leading spaces on each line are converted.

## CURRENT CAPABILITIES

- converting leading spaces to tabs (default)
- converting all space runs to tabs with `-a`
- skipping non-initial spaces with `-i`
- custom single or multiple tab stop positions
- NUL-delimited record mode with `-z`
- space-to-tab conversion based on shared UTF-8 display-width handling for common combining marks, ANSI escapes, and wide East Asian/emoji characters

## OPTIONS

- `-a` — convert all runs of spaces, not only leading ones
- `-i` — convert only the initial (leading) spaces on each line (default behaviour; this flag makes the intent explicit)
- `-z`, `--zero-terminated` — treat NUL as a record separator for column resets
- `-t TABSTOP` — use a single tab stop width of TABSTOP columns
- `-t LIST` — use a comma-separated list of absolute column positions as tab stops

## LIMITATIONS

- mixed space-and-tab indentation may not be fully collapsed in all cases
- tab stop positions are 1-based column numbers
- display width uses compact Unicode/default-width tables, not locale-specific width data; ambiguous-width characters and full grapheme clusters may not match every terminal
- locale-specific tab semantics are not implemented

## EXAMPLES

- `unexpand file.txt` — convert leading spaces to tabs (8-column stops)
- `unexpand -t 4 file.txt` — use 4-column tab stops
- `unexpand -a file.txt` — convert all space runs
- `unexpand -z -a -t 4 paths0.txt` — convert NUL-delimited records

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

expand, cat, pr
