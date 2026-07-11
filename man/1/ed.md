# ED

## NAME

ed - line-oriented text editor

## SYNOPSIS

```
ed [file]
```

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
- regex character classes, groups, alternation, quantifiers, and replacement
  back-references through the shared project regex engine
- line address ranges (`1,5p`, `.,+3d`, `%p`)
- stepped address ranges (`addr,~step`)
- global commands (`g/pattern/command`) for `p`, `n`, `d`, and `s`
- undoing the previous buffer-changing command (`u`)
- reading additional content into the buffer (`r`)

## OPTIONS

ed accepts no flags other than an optional filename argument. `--help` prints usage.

## LIMITATIONS

- the buffer is line-oriented and grows through the runtime allocator; very large edits and undo snapshots are limited by available memory
- global commands support the common scripted edit operations `p`, `n`, `d`,
  and `s`; nested command lists are not implemented
- full interactive diagnostics and recovery behavior are intentionally compact

## EXAMPLES

- `ed file.txt` — open file for editing
- `1,$p` — print all lines
- `3,5d` — delete lines 3 through 5
- `1,$ s/foo/bar/g` — replace all occurrences of foo with bar
- `g/^#/d` — delete all comment lines
- `u` — undo the previous buffer-changing command
- `w` — write the buffer back to disk

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

sed, patch, vi
