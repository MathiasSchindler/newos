# ED

## NAME

ed - line-oriented text editor

## SYNOPSIS

ed [file]

## DESCRIPTION

ed is a line-oriented text editor. It reads commands from standard input and operates on an in-memory buffer that can be loaded from and saved to a file. ed is the standard Unix editor and is useful for scripted editing.

## CURRENT CAPABILITIES

- reading a file into the buffer on startup
- navigating by line number and relative offsets (`+`, `-`)
- appending, inserting, and changing lines (`a`, `i`, `c`)
- deleting lines (`d`)
- printing lines (`p`, `n`, `l`)
- writing the buffer to a file (`w`)
- quitting with and without saving (`q`, `Q`)
- substitution with `s/old/new/` and `s/old/new/g`
- line address ranges (`1,5p`, `.,+3d`, `%p`)
- reading additional content into the buffer (`r`)

## OPTIONS

ed accepts no flags other than an optional filename argument. `--help` prints usage.

## LIMITATIONS

- no support for regex character classes, back-references, or ERE syntax in search/substitute patterns
- no `g` (global) command for applying commands to matching lines
- no undo (`u`) command
- no extended address forms such as `addr,~step`

## EXAMPLES

- `ed file.txt` — open file for editing
- `1,$p` — print all lines
- `3,5d` — delete lines 3 through 5
- `1,$ s/foo/bar/g` — replace all occurrences of foo with bar
- `w` — write the buffer back to disk

## SEE ALSO

sed, patch, vi
