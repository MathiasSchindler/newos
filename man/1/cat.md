# CAT

## NAME

cat - concatenate files and display them on standard output

## SYNOPSIS

```
cat [-n] [-b] [-s] [-u] [-v] [-E] [-T] [-A] [file ...]
```

## DESCRIPTION

`cat` streams one or more files to standard output and can annotate or render
input in more visible forms for inspection.

## CURRENT CAPABILITIES

- concatenate multiple files or standard input
- number all lines with `-n`
- number non-blank lines with `-b`
- squeeze repeated blank lines with `-s`
- show tabs, line ends, and non-printing characters with `-T`, `-E`, `-v`, and `-A`

## OPTIONS

- `-n` number all output lines
- `-b` number only non-blank lines
- `-s` squeeze adjacent blank lines
- `-u` unbuffered-style streaming behavior
- `-v`, `-E`, `-T`, `-A` make content more visible

## LIMITATIONS

- the implementation focuses on the currently supported visibility and numbering modes
- compatibility with every GNU/BSD flag combination is not guaranteed
- no long-option aliases such as `--show-all`, `--number`, or
  `--squeeze-blank` are provided yet
- output transformation is byte-oriented; it does not understand terminal
  display width, combining characters, or locale-specific printable classes

## EXAMPLES

```
cat file.txt
cat -n notes.txt
cat -A config.txt
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

head, tail, wc, less
