# LESS

## NAME

less - page through text one screen at a time

## SYNOPSIS

less [-N] [file ...]

## DESCRIPTION

less displays text files (or standard input) one screen at a time, pausing for input after each page. Press space or Enter to advance, `q` to quit. Unlike more, less supports backward navigation within a page prompt via the `b` key.

## CURRENT CAPABILITIES

- forward and backward paging through files
- optional line numbering
- reading from standard input when no files are given
- reading from multiple files in sequence

## OPTIONS

- `-N` — prefix each line with its line number

## LIMITATIONS

- no search (`/pattern`) within the pager
- no syntax highlighting or colour support
- no scroll-by-line arrow-key navigation within the pager prompt; only full-page and single-line (`Enter`) advances are supported
- does not support all terminal control codes of the reference less(1)

## EXAMPLES

- `less file.txt` — page through a file
- `less -N file.txt` — page through a file with line numbers
- `cat long.log | less` — page through piped output

## SEE ALSO

more, man, cat, head, tail
