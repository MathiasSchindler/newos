# FOLD

## NAME

fold - wrap long lines to a specified width

## SYNOPSIS

```
fold [-bcs] [-w WIDTH] [file ...]
```

## DESCRIPTION

fold reads files (or standard input) and wraps lines that exceed WIDTH characters (default 80) by inserting a newline.

## CURRENT CAPABILITIES

- wrapping at a configurable column width
- byte-based wrapping instead of character-based
- breaking only at whitespace to avoid splitting words

## OPTIONS

- `-b` — count bytes instead of characters when measuring line width
- `-c` — same as `-b` (alternative byte-mode flag)
- `-s` — break at the last whitespace at or before WIDTH rather than in the middle of a word
- `-w WIDTH` — wrap at WIDTH columns (default 80)

## LIMITATIONS

- multi-byte UTF-8 characters are counted as multiple bytes unless byte mode is in effect; display width of wide characters is not considered
- no colour-sequence awareness (ANSI escape codes count toward column width)

## EXAMPLES

- `fold -w 72 essay.txt` — wrap at 72 columns
- `fold -s -w 60 text.txt` — wrap at 60 columns, breaking at spaces
- `echo "a very long line" | fold -b -w 10` — byte-based wrap at 10

## SEE ALSO

fmt, cut, pr
