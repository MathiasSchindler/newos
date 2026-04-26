# CUT

## NAME

cut - extract fields or columns from lines

## SYNOPSIS

```
cut [--complement] (-b LIST | -c LIST | -f LIST [-d DELIM]) [file ...]
```

## DESCRIPTION

The cut tool extracts selected bytes, character positions, or delimited fields from each input line. Ranges are 1-based and can be combined with commas.

## CURRENT CAPABILITIES

- select byte or character ranges
- extract delimited fields with `-f`
- use a custom delimiter with `-d`
- invert the selection with `--complement`

## OPTIONS

| Flag | Description |
|------|-------------|
| `-b LIST` | Select byte ranges. |
| `-c LIST` | Select character ranges. |
| `-f LIST` | Select 1-based field ranges. |
| `-d DELIM` | Use `DELIM` instead of tab as the field separator. |
| `--complement` | Invert the selection. |

## LIMITATIONS

- At most 32 ranges are supported.
- Maximum line length is 8192 bytes.
- No NUL-delimited `-z` mode is implemented.
- `-c` and `-b` currently behave the same and operate byte-by-byte.

## EXAMPLES

```
cut -f1 data.tsv
cut -d : -f1,7 /etc/passwd
cut --complement -c1-8 text.txt
```

## SEE ALSO

sort, head, tail, wc
