# CUT

## NAME

cut - extract fields or columns from lines

## SYNOPSIS

```
cut [--complement] [-z|--zero-terminated] (-b LIST | -c LIST | -f LIST [-d DELIM]) [file ...]
```

## DESCRIPTION

The cut tool extracts selected bytes, character positions, or delimited fields from each input line. Ranges are 1-based and can be combined with commas.

## CURRENT CAPABILITIES

- select byte or character ranges
- select extended grapheme-cluster positions with `-c`
- extract delimited fields with `-f`
- use a custom delimiter with `-d`
- process NUL-terminated records with `-z`
- invert the selection with `--complement`

## OPTIONS

| Flag | Description |
|------|-------------|
| `-b LIST` | Select byte ranges. |
| `-c LIST` | Select character ranges. |
| `-f LIST` | Select 1-based field ranges. |
| `-d DELIM` | Use `DELIM` instead of tab as the field separator. |
| `-z`, `--zero-terminated` | Use NUL instead of newline as the input and output record separator. |
| `--complement` | Invert the selection. |

## LIMITATIONS

- `-c` keeps common combining, emoji-modifier, flag, Hangul, and zero-width-joiner sequences together using the shared compact grapheme iterator.
- `-b` remains byte-oriented and can split a UTF-8 sequence by design.
- field delimiters are single bytes; multibyte delimiters and locale-specific character classes are not implemented.
- no locale database or normalization-aware matching is used.

## EXAMPLES

```
cut -f1 data.tsv
cut -d : -f1,7 /etc/passwd
cut -z -d : -f2 records.bin
cut --complement -c1-8 text.txt
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

sort, head, tail, wc
