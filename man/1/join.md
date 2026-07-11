# JOIN

## NAME

join - join lines of two files on a common field

## SYNOPSIS

```
join [-i] [-a1|-a 1] [-a2|-a 2] [-v1|-v 1] [-v2|-v 2]
	[-1 FIELD] [-2 FIELD] [-j FIELD] [-t CHAR] [-e EMPTY] [-o LIST]
	file1 file2
```

## DESCRIPTION

join reads two files sorted on a join field and writes one line per matching pair. Unmatched lines may optionally be included with `-a` or `-v`.

## CURRENT CAPABILITIES

- inner join of two pre-sorted files on a configurable field
- printing unmatched lines from either or both files
- case-insensitive matching
- custom field delimiter
- custom output field list
- substitution string for missing fields

## OPTIONS

- `-i` / `--ignore-case` — compare join fields case-insensitively
- `-a1` / `-a 1` — also print unmatched lines from file1
- `-a2` / `-a 2` — also print unmatched lines from file2
- `-v1` / `-v 1` — print only unmatched lines from file1 (suppress joined output)
- `-v2` / `-v 2` — print only unmatched lines from file2
- `-1 FIELD` — join on field FIELD in file1 (1-based)
- `-2 FIELD` — join on field FIELD in file2 (1-based)
- `-j FIELD` — shorthand for `-1 FIELD -2 FIELD`
- `-t CHAR` — use CHAR as the field delimiter (default: whitespace)
- `-e EMPTY` — substitute EMPTY for missing output fields
- `-o LIST` — select output fields as a comma-separated list (e.g. `1.2,2.3,0`)

## LIMITATIONS

- both input files must be sorted on the join field before running join
- no support for reading from standard input via `-`
- field parsing is byte-oriented and does not implement locale collation,
  multibyte blank handling, or CSV-style quoting
- output formatting supports the documented common cases, not the full GNU
  `-o FORMAT` grammar

## EXAMPLES

- `join names.txt scores.txt` — join on the first field
- `join -1 2 -2 1 a.txt b.txt` — join on field 2 of a and field 1 of b
- `join -t: /etc/passwd /etc/shadow` — join colon-delimited files
- `join -a1 -e N/A -o 1.1,2.2 a.txt b.txt` — left join with a fill value

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

sort, paste, cut, comm
