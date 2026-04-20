# SORT

## NAME

sort - sort text lines

## SYNOPSIS

sort [-bCcfmnrsu] [-o FILE] [-t CHAR] [-k FIELD[,FIELD]] [file ...]

## DESCRIPTION

The sort tool orders text lines lexically or numerically. It supports reverse
order, deduplication, case-folding, sortedness checks, field-based keys, and
memory-friendlier merge operation for already sorted inputs.

## CURRENT CAPABILITIES

- sort lines lexically by default
- sort numerically with `-n`, including values larger than machine-sized integers
- reverse output or keep only unique lines with `-r` and `-u`
- ignore leading blanks with `-b` and ASCII case with `-f`
- preserve stable ordering for equal keys
- use a custom field separator and key range
- merge already sorted input sets with `-m` using streaming input
- check whether input is already sorted with `-c` or quiet `-C`
- write output directly to a chosen file with `-o FILE`
- read from one or more files or standard input

## OPTIONS

| Flag | Description |
|------|-------------|
| `-b` | Ignore leading blanks in comparisons. |
| `-c` | Check whether input is sorted; exit non-zero on the first disorder. |
| `-C` | Like `-c`, but stay quiet unless a read error occurs. |
| `-f` | Fold ASCII lower/upper case together during comparisons. |
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
- Normal sorting still buffers input in memory before writing output, although
  `-m` and `-c` now avoid that for common streaming workflows.
- No `--parallel`, month sort, locale collation, or GNU-style human-numeric sort
  modes are implemented.

## EXAMPLES

- `sort names.txt`
- `sort -n scores.txt`
- `sort -c already.sorted`
- `sort -f names.txt`
- `sort -u words.txt`
- `sort -t : -k 3,3 /etc/passwd`
- `sort -m part1.sorted part2.sorted`
- `sort -o final.txt input.txt`

## SEE ALSO

cut, uniq, wc
