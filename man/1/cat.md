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

## EXAMPLES

```
cat file.txt
cat -n notes.txt
cat -A config.txt
```

## SEE ALSO

head, tail, wc, less
