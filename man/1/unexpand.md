# UNEXPAND

## NAME

unexpand - convert spaces to tabs

## SYNOPSIS

unexpand [-a] [-i] [-t TABSTOP[,TABSTOP...]] [file ...]

## DESCRIPTION

unexpand reads files (or standard input) and converts runs of spaces back to tab characters where they align with tab stops. By default only leading spaces on each line are converted.

## CURRENT CAPABILITIES

- converting leading spaces to tabs (default)
- converting all space runs to tabs with `-a`
- skipping non-initial spaces with `-i`
- custom single or multiple tab stop positions

## OPTIONS

- `-a` — convert all runs of spaces, not only leading ones
- `-i` — convert only the initial (leading) spaces on each line (default behaviour; this flag makes the intent explicit)
- `-t TABSTOP` — use a single tab stop width of TABSTOP columns
- `-t LIST` — use a comma-separated list of absolute column positions as tab stops

## LIMITATIONS

- mixed space-and-tab indentation may not be fully collapsed in all cases
- tab stop positions are 1-based column numbers

## EXAMPLES

- `unexpand file.txt` — convert leading spaces to tabs (8-column stops)
- `unexpand -t 4 file.txt` — use 4-column tab stops
- `unexpand -a file.txt` — convert all space runs

## SEE ALSO

expand, cat, pr
