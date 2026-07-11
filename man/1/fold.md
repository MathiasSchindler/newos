# FOLD

## NAME

fold - wrap long lines to a specified width

## SYNOPSIS

```
fold [-bcs] [-w WIDTH] [file ...]
```

## DESCRIPTION

fold reads files (or standard input) and wraps lines that exceed WIDTH display columns (default 80) by inserting a newline.

## CURRENT CAPABILITIES

- wrapping at a configurable display-column width
- UTF-8 decoding with wide and zero-width character display widths
- byte-based wrapping with `-b`
- grapheme-cluster-count wrapping with `-c`
- breaking only at Unicode whitespace to avoid splitting words
- ANSI CSI and OSC escape sequences are preserved and treated as display-width-neutral in character and column modes

## OPTIONS

- `-b` — count bytes instead of display columns when measuring line width
- `-c` — count UTF-8 characters instead of display columns when measuring line width
- `-s` — break at the last whitespace at or before WIDTH rather than in the middle of a word
- `-w WIDTH` — wrap at WIDTH columns (default 80)

## LIMITATIONS

- byte mode may split inside multi-byte UTF-8 characters because it deliberately counts raw bytes
- East Asian Ambiguous characters default to width 1; `NEWOS_AMBIGUOUS_WIDTH=2` selects width 2
- compact grapheme segmentation covers combining marks, emoji modifiers, regional-indicator flags, Hangul sequences, and ZWJ sequences without a full Unicode property database
- terminal control handling is limited to common ANSI CSI and OSC escape sequences plus basic carriage-return/backspace width effects

## EXAMPLES

- `fold -w 72 essay.txt` — wrap at 72 columns
- `fold -s -w 60 text.txt` — wrap at 60 columns, breaking at spaces
- `echo "a very long line" | fold -b -w 10` — byte-based wrap at 10

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

fmt, cut, pr
