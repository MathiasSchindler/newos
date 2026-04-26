# EXPAND

## NAME

expand - convert tabs to spaces

## SYNOPSIS

```
expand [-i] [-t TABSTOP[,TABSTOP...]] [file ...]
```

## DESCRIPTION

expand reads files (or standard input) and writes them with tab characters replaced by the appropriate number of spaces to reach the next tab stop.

## CURRENT CAPABILITIES

- converting all tabs to spaces with default 8-column tab stops
- expanding only leading tabs with `-i`
- custom single or multiple tab stop positions

## OPTIONS

- `-i` — expand only the leading tabs on each line; tabs after the first non-whitespace character are left unchanged
- `-t TABSTOP` — use a single tab stop width of TABSTOP columns (e.g. `-t 4` for 4-space tabs)
- `-t LIST` — use a comma-separated list of absolute column positions as tab stops; tabs are expanded to reach the next listed stop, or every 1 column after the last stop

## LIMITATIONS

- tab stop positions are 1-based column numbers
- no support for tab stop specification via repeated `-t` flags (a single `-t` argument is required)

## EXAMPLES

- `expand file.txt` — expand tabs to 8-space stops
- `expand -t 4 file.txt` — expand to 4-space stops
- `expand -i -t 2 file.txt` — expand only leading tabs to 2-space stops
- `expand -t 1,9,17 file.txt` — expand to explicit stop positions

## SEE ALSO

unexpand, cat, pr
