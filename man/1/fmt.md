# FMT

## NAME

fmt - simple optimal text formatter

## SYNOPSIS

```
fmt [-w WIDTH] [-s] [-u] [-c] [-p PREFIX] [file ...]
```

## DESCRIPTION

fmt fills and joins short lines in paragraphs to produce output with lines no longer than WIDTH (default 75). Paragraphs are separated by blank lines or changes in indentation.

## CURRENT CAPABILITIES

- reflowing paragraphs to a target line width
- split-only mode that breaks long lines without joining short ones
- uniform spacing (single space between words, two after sentence-ending punctuation)
- crown-margin mode that preserves the indentation of the first two lines
- prefix-aware formatting (lines sharing a common prefix are reflowed together)

## OPTIONS

- `-w WIDTH` / `--width WIDTH` — target line width in characters (default 75)
- `-s` / `--split-only` — split lines longer than WIDTH but do not join short lines
- `-u` / `--uniform-spacing` — normalise inter-word spacing to one space (two after sentence end)
- `-c` / `--crown-margin` — preserve the indentation of the first two lines as the paragraph margin
- `-p PREFIX` / `--prefix PREFIX` — reformat only lines beginning with PREFIX, keeping the prefix on output

## LIMITATIONS

- does not handle tab-expanded widths when measuring indentation
- no support for `-t` (tagged paragraph mode)
- sentence detection is heuristic (word ends with `.`, `?`, `!`, or `:`)

## EXAMPLES

- `fmt -w 72 prose.txt` — reformat to 72-column paragraphs
- `fmt -s -w 80 code_comments.txt` — break long lines only
- `fmt -u essay.txt` — normalise spacing

## SEE ALSO

fold, pr, sed
