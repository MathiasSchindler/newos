# SORT

## NAME

sort - sort text lines

## SYNOPSIS

sort [-bCcdfiMmnrsuV] [-o FILE] [-t CHAR] [-k FIELD[,FIELD]] [file ...]
sort --human-numeric-sort [-bCcfmnrsu] [-o FILE] [-t CHAR] [-k FIELD[,FIELD]] [file ...]
sort -h
sort --help

## DESCRIPTION

The sort tool orders text lines lexically or numerically. It reads one or more
files, or standard input when no file is named or `-` is used. It supports
reverse order, deduplication, case-folding, dictionary filtering, sortedness
checks, month/version/human-size ordering, field-based keys, direct output to a
file, and memory-friendlier merge operation for already sorted inputs.

## CURRENT CAPABILITIES

- sort lines lexically by default
- sort numerically with `-n`, including values larger than machine-sized integers
- sort human-readable sizes with `--human-numeric-sort`
- sort month names with `-M` and version-like strings with `-V`
- reverse output or keep only unique lines with `-r` and `-u`
- ignore leading blanks with `-b`, ASCII case with `-f`, non-dictionary
  characters with `-d`, and non-printing bytes with `-i`
- preserve stable ordering for equal keys
- use blank-separated fields, or a custom field separator and key range
- merge already sorted input sets with `-m` using streaming input
- check whether input is already sorted with `-c` or quiet `-C`
- write output directly to a chosen file with `-o FILE`
- read from one or more files or standard input
- print the usage line with `-h` or `--help`

## OPTIONS

| Flag | Description |
|------|-------------|
| `-b` | Ignore leading blanks in comparisons. |
| `-c` | Check whether input is sorted; exit non-zero on the first disorder. |
| `-C` | Like `-c`, but stay quiet unless a read error occurs. |
| `-d` | Compare only ASCII letters, digits, spaces, and tabs. |
| `-f` | Fold ASCII lower/upper case together during comparisons. |
| `-i` | Ignore non-printing ASCII bytes during comparisons. |
| `-M` | Compare the first three non-blank letters as month names. |
| `-m` | Merge sorted inputs into sorted output. |
| `-n` | Compare according to numeric value. |
| `-r` | Reverse the sort order. |
| `-s` | Request stable ordering for equal keys. |
| `-u` | Output unique lines only. |
| `-V` | Compare embedded digit runs as version numbers. |
| `-o FILE` | Write the sorted result to FILE instead of standard output. |
| `-t CHAR` | Use `CHAR` as the field separator. |
| `-k FIELD[,FIELD]` | Sort using a 1-based field or field range. |
| `--dictionary-order` | Long form of `-d`. |
| `--human-numeric-sort` | Compare scaled values such as `950`, `1K`, `1.5M`, or `2GiB`. |
| `--ignore-nonprinting` | Long form of `-i`. |
| `--month-sort` | Long form of `-M`. |
| `--version-sort` | Long form of `-V`. |
| `-h`, `--help` | Print a short usage line. |

Without `-t`, fields are runs of non-blank text separated by spaces or tabs.
With `-t`, every instance of `CHAR` separates fields. `FIELD` and `FIELD,FIELD`
are 1-based; overflowing, zero, or descending key ranges are rejected.

Numeric comparisons accept optional leading blanks, an optional sign, decimal
digits, and an optional fractional part. If either compared key is not a complete
numeric value, comparison falls back to bytewise text ordering for that pair.

Human-size comparisons accept the same numeric prefix followed by optional
binary scale suffixes `K`, `M`, `G`, `T`, `P`, or `E`, with optional `i` and `B`
suffix letters. Version comparisons compare digit runs by numeric value, so
`file-2` sorts before `file-10`. Month comparisons recognize English month names
by their first three letters after leading blanks.

## LIMITATIONS

- Comparisons are bytewise rather than locale-aware.
- `-u` removes duplicate output lines after sorting; it is not a replacement for
  `uniq` when you need adjacent-group counts.
- Normal sorting uses bounded in-memory chunks of up to 8192 lines, 64 KiB per
  line, and 2 MiB of stored text, then spills additional sorted chunks to
  temporary files under `/tmp` and merges them back. This keeps hosted and
  freestanding behavior identical while allowing much larger inputs than one
  chunk.
- A single line is still limited to 64 KiB. The external sorter keeps up to 128
  temporary runs before reporting "too many temporary runs".
- Merge mode keeps fixed per-input state and currently accepts up to eight input
  files at a time. If `-o FILE` names one of the input paths, sort buffers first
  so the input is not truncated before it is read.
- Human-size arithmetic saturates at the largest unsigned machine value for
  extremely large scaled inputs.
- No `--parallel` mode or locale collation is implemented.

## EXAMPLES

- `sort names.txt`
- `sort -n scores.txt`
- `sort --human-numeric-sort sizes.txt`
- `sort -c already.sorted`
- `sort -f names.txt`
- `sort -d dictionary.txt`
- `sort -M months.txt`
- `sort -V releases.txt`
- `sort -u words.txt`
- `sort large-input.txt > sorted.txt`
- `sort -t : -k 3,3 /etc/passwd`
- `sort -n -t : -k 3,3 /etc/passwd`
- `sort -m part1.sorted part2.sorted`
- `sort -o final.txt input.txt`

## SEE ALSO

cut, uniq, wc
