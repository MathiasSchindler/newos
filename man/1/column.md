# COLUMN

## NAME

column - format input into columns

## SYNOPSIS

```
column [-t] [-x] [-s SEP] [-o OUTSEP] [-c WIDTH] [file ...]
```

## DESCRIPTION

column reads lines from files or standard input and arranges them into a table. In the default mode it packs items into columns to fit the terminal width. In table mode (`-t`) each input field is aligned into its own column.

## CURRENT CAPABILITIES

- default mode: arrange whitespace-delimited tokens into evenly spaced columns
- table mode: align delimited fields into padded columns
- fill by column before rows with `-x`
- custom input and output field separators
- configurable output width
- alignment based on shared UTF-8 display-width handling for common combining marks, ANSI escapes, and wide East Asian/emoji characters

## OPTIONS

- `-t` — table mode: parse each line as delimited fields and align them into columns
- `-x` — fill columns before filling rows (column-major order)
- `-s SEP` — use the characters in SEP as input field separators (default: whitespace)
- `-o OUTSEP` — use OUTSEP as the output separator between columns (default: two spaces)
- `-c WIDTH` — use WIDTH as the target output width (default: terminal width or 80)

## LIMITATIONS

- no support for right-alignment or per-column width overrides
- no `-J` (JSON output) or `-R` (right-align) flags found in util-linux column
- display width uses compact Unicode tables, not locale data; East Asian Ambiguous characters default to width 1 and use width 2 when `NEWOS_AMBIGUOUS_WIDTH=2`

## EXAMPLES

- `column -t /etc/fstab` — align fstab fields
- `ls | column -c 60` — fit ls output into 60 columns
- `column -t -s: /etc/passwd` — table from colon-delimited file
- `column -t -s: -o' | ' /etc/passwd` — pipe-separated table

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

paste, pr, awk, cut
