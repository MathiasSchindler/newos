# PASTE

## NAME

paste - merge lines of files side by side

## SYNOPSIS

```
paste [-sz] [-d DELIMS] [file ...]
```

## DESCRIPTION

paste reads corresponding lines from multiple files and writes them joined by a delimiter. In serial mode (`-s`) lines from each file are joined into one output line instead.

## CURRENT CAPABILITIES

- parallel merge of lines from multiple files (default mode)
- serial merge of all lines from each file into one line
- configurable multi-character delimiter cycle
- NUL-terminated record mode
- reading from standard input using `-` as a filename

## OPTIONS

- `-s` — serial mode: concatenate all lines of each file into a single line before merging files
- `-z` — use NUL as the line terminator instead of newline
- `-d DELIMS` — delimiter string; characters are used cyclically to separate fields (default tab)

## LIMITATIONS

- maximum number of open files and line length are bounded by internal static buffers
- no support for multi-byte delimiter sequences (each character in DELIMS is a single-byte delimiter)

## EXAMPLES

- `paste file1 file2` — merge files side by side with a tab
- `paste -d: /etc/group /etc/passwd` — merge with colon delimiter
- `paste -s file.txt` — merge all lines into one
- `paste - - < file` — merge pairs of lines

## SEE ALSO

join, cut, pr, column
