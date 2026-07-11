# REV

## NAME

rev - reverse the characters in each line

## SYNOPSIS

```
rev [-0] [file ...]
```

## DESCRIPTION

rev reads lines from files or standard input and writes each line with its grapheme clusters reversed.

## CURRENT CAPABILITIES

- reversing characters in each line of a file or standard input
- shared grapheme-aware reversal covering combining marks, variation selectors,
  emoji modifiers, regional-indicator flags, Hangul, and zero-width-joiner sequences
- ANSI escape/control sequences are preserved as atomic spans
- NUL-delimited record mode

## OPTIONS

- `-0` — use NUL as the record terminator instead of newline

## LIMITATIONS

- no support for right-to-left text aware reversal
- rare grapheme rules outside the compact in-tree property tables are not implemented

## EXAMPLES

- `rev file.txt` — reverse each line of a file
- `echo "hello" | rev` — prints `olleh`
- `rev -0 nul-records` — reverse NUL-delimited records

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

tac, tr, cut
