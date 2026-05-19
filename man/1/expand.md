# EXPAND

## NAME

expand - convert tabs to spaces

## SYNOPSIS

```
expand [-iz] [-t TABSTOP[,TABSTOP...]] [file ...]
```

## DESCRIPTION

expand reads files (or standard input) and writes them with tab characters replaced by the appropriate number of spaces to reach the next tab stop.

## CURRENT CAPABILITIES

- converting all tabs to spaces with default 8-column tab stops
- expanding only leading tabs with `-i`
- custom single or multiple tab stop positions
- tab expansion based on shared UTF-8 display-width handling for common combining marks, ANSI escapes, and wide East Asian/emoji characters
- NUL-delimited record mode with `-z`

## OPTIONS

- `-i` — expand only the leading tabs on each line; tabs after the first non-whitespace character are left unchanged
- `-z`, `--zero-terminated` — treat NUL as a record separator for column resets
- `-t TABSTOP` — use a single tab stop width of TABSTOP columns (e.g. `-t 4` for 4-space tabs)
- `-t LIST` — use a comma-separated list of absolute column positions as tab stops; tabs are expanded to reach the next listed stop, or every 1 column after the last stop

## LIMITATIONS

- tab stop positions are 1-based column numbers
- no support for tab stop specification via repeated `-t` flags (a single `-t` argument is required)
- display width uses compact Unicode/default-width tables, not locale-specific width data; ambiguous-width characters and full grapheme clusters may not match every terminal
- locale-specific tab semantics are not implemented

## EXAMPLES

- `expand file.txt` — expand tabs to 8-space stops
- `expand -t 4 file.txt` — expand to 4-space stops
- `expand -i -t 2 file.txt` — expand only leading tabs to 2-space stops
- `expand -z -t 4 paths0.txt` — reset columns at NUL-delimited records
- `expand -t 1,9,17 file.txt` — expand to explicit stop positions

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

unexpand, cat, pr
