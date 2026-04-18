# WC

## NAME

wc - count lines, words, characters, and bytes

## SYNOPSIS

wc [-lwcmL] [file ...]

## DESCRIPTION

The wc tool counts lines, words, bytes, characters, and maximum line length. With no flags it prints lines, words, and bytes.

## CURRENT CAPABILITIES

- count lines, words, and bytes by default
- count UTF-8 characters with `-m`
- report maximum line length with `-L`
- read from files or standard input

## OPTIONS

| Flag | Description |
|------|-------------|
| `-l` | Print the line count. |
| `-w` | Print the word count. |
| `-c` | Print the byte count. |
| `-m` | Print the character count using UTF-8 decoding. |
| `-L` | Print the maximum line length. |

## LIMITATIONS

- Character counting with `-m` is UTF-8-based and may be wrong for other encodings.
- No `-0` mode is implemented.

## EXAMPLES

- `wc file.txt`
- `wc -l *.log`
- `wc -m unicode.txt`

## SEE ALSO

cat, head, tail, sort
