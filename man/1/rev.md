# REV

## NAME

rev - reverse the characters in each line

## SYNOPSIS

```
rev [-0] [file ...]
```

## DESCRIPTION

rev reads lines from files or standard input and writes each line with its characters reversed. UTF-8 multi-byte characters are kept intact as units during reversal.

## CURRENT CAPABILITIES

- reversing characters in each line of a file or standard input
- UTF-8-aware reversal (multi-byte sequences are not split)
- NUL-delimited record mode

## OPTIONS

- `-0` — use NUL as the record terminator instead of newline

## LIMITATIONS

- combining characters (Unicode code points that modify a preceding base character) are reversed in sequence; composed grapheme clusters are not preserved as atomic units
- no support for right-to-left text aware reversal

## EXAMPLES

- `rev file.txt` — reverse each line of a file
- `echo "hello" | rev` — prints `olleh`
- `rev -0 nul-records` — reverse NUL-delimited records

## SEE ALSO

tac, tr, cut
