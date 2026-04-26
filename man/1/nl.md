# NL

## NAME

nl - number lines of files

## SYNOPSIS

```
nl [-ba|-bt|-bn] [-h STYLE] [-b STYLE] [-f STYLE] [-d CC]
	[-n FORMAT] [-v START] [-i INCREMENT] [-w WIDTH] [-s SEP] [file ...]
```

## DESCRIPTION

nl reads files (or standard input) and writes them to standard output with line numbers prepended. It supports logical page sections (header, body, footer) separated by special delimiter lines.

## CURRENT CAPABILITIES

- numbering body, header, and footer sections independently
- three numbering styles per section: `a` (all lines), `t` (non-empty lines), `n` (no numbering)
- configurable starting line number, increment, and number width
- configurable number format: `ln` (left-justified), `rn` (right-justified, no padding), `rz` (right-justified, zero-padded)
- configurable separator between number and line text
- logical page delimiter detection and optional customisation

## OPTIONS

- `-b STYLE` — body section numbering style (`a`, `t`, or `n`); shorthand: `-ba`, `-bt`, `-bn`
- `-h STYLE` — header section numbering style; shorthand: `-ha`, `-ht`, `-hn`
- `-f STYLE` — footer section numbering style; shorthand: `-fa`, `-ft`, `-fn`
- `-d CC` — use CC (two characters) as the logical page delimiter (default `\:`)
- `-n FORMAT` — number format: `ln`, `rn`, or `rz`
- `-v START` — starting line number (default 1)
- `-i INCREMENT` — line number increment (default 1)
- `-w WIDTH` — minimum field width for the line number (default 6)
- `-s SEP` — separator between number and text (default tab)

## LIMITATIONS

- logical page restart on each new page section header (`\:\:\:`) is honoured but resets only when explicitly present
- no `-p` flag to suppress page restarts as in GNU nl

## EXAMPLES

- `nl file.txt` — number non-empty body lines
- `nl -ba file.txt` — number all lines including blank ones
- `nl -v0 -i2 file.txt` — start at 0 and increment by 2
- `nl -n rz -w 4 file.txt` — zero-padded 4-digit numbers

## SEE ALSO

cat, wc, pr
