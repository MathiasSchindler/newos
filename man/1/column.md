# COLUMN

## NAME

column - format input into columns

## SYNOPSIS

column [-t] [-x] [-s SEP] [-o OUTSEP] [-c WIDTH] [file ...]

## DESCRIPTION

column reads lines from files or standard input and arranges them into a table. In the default mode it packs items into columns to fit the terminal width. In table mode (`-t`) each input field is aligned into its own column.

## CURRENT CAPABILITIES

- default mode: arrange whitespace-delimited tokens into evenly spaced columns
- table mode: align delimited fields into padded columns
- fill by column before rows with `-x`
- custom input and output field separators
- configurable output width

## OPTIONS

- `-t` — table mode: parse each line as delimited fields and align them into columns
- `-x` — fill columns before filling rows (column-major order)
- `-s SEP` — use the characters in SEP as input field separators (default: whitespace)
- `-o OUTSEP` — use OUTSEP as the output separator between columns (default: two spaces)
- `-c WIDTH` — use WIDTH as the target output width (default: terminal width or 80)

## LIMITATIONS

- maximum number of rows and fields per row is bounded by internal static buffers
- no support for right-alignment or per-column width overrides
- no `-J` (JSON output) or `-R` (right-align) flags found in util-linux column

## EXAMPLES

- `column -t /etc/fstab` — align fstab fields
- `ls | column -c 60` — fit ls output into 60 columns
- `column -t -s: /etc/passwd` — table from colon-delimited file
- `column -t -s: -o' | ' /etc/passwd` — pipe-separated table

## SEE ALSO

paste, pr, awk, cut
