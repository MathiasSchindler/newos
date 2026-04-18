# SORT

## NAME

sort - sort text lines

## SYNOPSIS

sort [-nru] [-t CHAR] [-k FIELD[,FIELD]] [file ...]

## DESCRIPTION

The sort tool orders text lines lexically or numerically. It also supports reverse order, deduplication, and simple field-based keys.

## CURRENT CAPABILITIES

- sort lines lexically by default
- sort numerically with `-n`
- reverse output or keep only unique lines
- use a custom field separator and key range

## OPTIONS

| Flag | Description |
|------|-------------|
| `-n` | Compare according to numeric value. |
| `-r` | Reverse the sort order. |
| `-u` | Output unique lines only. |
| `-t CHAR` | Use `CHAR` as the field separator. |
| `-k FIELD[,FIELD]` | Sort using a 1-based field or field range. |

## LIMITATIONS

- Input is limited to 2048 lines.
- Maximum line length is 512 bytes.
- Stable sort behavior is not guaranteed.
- No `--parallel` or merge mode (`-m`) is implemented.

## EXAMPLES

- `sort names.txt`
- `sort -n scores.txt`
- `sort -t : -k 3,3 /etc/passwd`

## SEE ALSO

cut, uniq, wc
