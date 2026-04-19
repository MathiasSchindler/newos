# SORT

## NAME

sort - sort text lines

## SYNOPSIS

sort [-mnrsu] [-o FILE] [-t CHAR] [-k FIELD[,FIELD]] [file ...]

## DESCRIPTION

The sort tool orders text lines lexically or numerically. It supports reverse order, deduplication, simple field-based keys, merge-style operation, and writing results directly to a chosen output file.

## CURRENT CAPABILITIES

- sort lines lexically by default
- sort numerically with `-n`, including values larger than machine-sized integers
- reverse output or keep only unique lines with `-r` and `-u`
- preserve stable ordering for equal keys
- use a custom field separator and key range
- merge already sorted input sets with `-m`
- write output directly to a chosen file with `-o FILE`
- read from one or more files or standard input

## OPTIONS

| Flag | Description |
|------|-------------|
| `-m` | Merge sorted inputs into sorted output. |
| `-n` | Compare according to numeric value. |
| `-r` | Reverse the sort order. |
| `-s` | Request stable ordering for equal keys. |
| `-u` | Output unique lines only. |
| `-o FILE` | Write the sorted result to FILE instead of standard output. |
| `-t CHAR` | Use `CHAR` as the field separator. |
| `-k FIELD[,FIELD]` | Sort using a 1-based field or field range. |

## LIMITATIONS

- Comparisons are bytewise rather than locale-aware.
- `-u` removes duplicate output lines after sorting; it is not a replacement for
  `uniq` when you need adjacent-group counts.
- The implementation currently buffers all input in memory before writing output,
  so very large datasets are still constrained by available memory.
- No `--parallel`, month sort, locale collation, or GNU-style human-numeric sort
  modes are implemented.

## EXAMPLES

- `sort names.txt`
- `sort -n scores.txt`
- `sort -u words.txt`
- `sort -t : -k 3,3 /etc/passwd`
- `sort -m part1.sorted part2.sorted`
- `sort -o final.txt input.txt`

## SEE ALSO

cut, uniq, wc
