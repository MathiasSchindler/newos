# WC

## NAME

wc - count lines, words, characters, and bytes

## SYNOPSIS

```
wc [-lwcmL] [file ...]
```

## DESCRIPTION

The wc tool counts lines, words, bytes, characters, and maximum line length. With no flags it prints lines, words, and bytes.

## CURRENT CAPABILITIES

- count lines, words, and bytes by default
- count UTF-8 characters with `-m`
- report maximum terminal display width with `-L`, including common combining marks, tabs, ANSI escapes, and wide East Asian/emoji characters
- read from files or standard input

## OPTIONS

| Flag | Description |
|------|-------------|
| `-l` | Print the line count. |
| `-w` | Print the word count. |
| `-c` | Print the byte count. |
| `-m` | Print the character count using UTF-8 decoding. |
| `-L` | Print the maximum terminal display width. |

## LIMITATIONS

- Character counting with `-m` is UTF-8-based and may be wrong for other encodings.
- No `-0` mode is implemented.
- display width (`-L`) uses compact in-tree Unicode tables, not a system locale database; ambiguous-width characters, grapheme clusters, and every terminal/emoji variation are not fully modeled.
- no NUL-delimited filename input mode or locale-specific word boundary rules
  are implemented.

## EXAMPLES

```
wc file.txt
wc -l *.log
wc -m unicode.txt
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

cat, head, tail, sort
