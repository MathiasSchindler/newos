# WC

## NAME

wc - count lines, words, characters, and bytes

## SYNOPSIS

```
wc [-lwcmL] [--json] [file ...]
```

## DESCRIPTION

The wc tool counts lines, words, bytes, characters, and maximum line length. With no flags it prints lines, words, and bytes.

## CURRENT CAPABILITIES

- count lines, words, and bytes by default
- count UTF-8 characters with `-m`
- report maximum terminal display width with `-L`, including common combining marks, tabs, ANSI escapes, emoji skin-tone modifiers, and wide East Asian/emoji characters
- read from files or standard input

## OPTIONS

| Flag | Description |
|------|-------------|
| `-l` | Print the line count. |
| `-w` | Print the word count. |
| `-c` | Print the byte count. |
| `-m` | Print the character count using UTF-8 decoding. |
| `-L` | Print the maximum terminal display width. |
| `--json` | Write newline-delimited JSON events. |

## LIMITATIONS

- Character counting with `-m` is UTF-8-based and may be wrong for other encodings.
- No `-0` mode is implemented.
- display width (`-L`) uses compact in-tree Unicode tables, not a system locale database; ambiguous-width characters, full grapheme clusters such as every ZWJ emoji sequence, and every terminal/emoji variation are not fully modeled.
- no NUL-delimited filename input mode or locale-specific word boundary rules
  are implemented.

## EXAMPLES

```
wc file.txt
wc -l *.log
wc -m unicode.txt
wc --json file.txt
```

## JSON Output

With `--json`, `wc` writes JSON Lines using the common envelope documented in `json-output`. Each processed file or stdin target emits a `wc_result` event on stdout. The `data` object contains:

- `file`: file path, or `null` for stdin
- `lines`: lines count
- `words`: words count
- `chars`: characters count
- `bytes`: bytes count
- `max_line_length`: maximum line length

When reading from multiple files, the accumulated counts are also written as a final `wc_result` event with `"file":"total"`. Usage warnings and errors are written to standard error as JSON diagnostic objects.

## SEE ALSO

cat, head, tail, sort
