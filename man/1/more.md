# MORE

## NAME

more - page through text one screen at a time (forward only)

## SYNOPSIS

more [file ...]

## DESCRIPTION

more displays text files (or standard input) one screen at a time, pausing after each page for the user to press space to continue or `q` to quit. It supports only forward paging.

## CURRENT CAPABILITIES

- forward paging through one or more files
- reading from standard input when no files are given
- per-file paging when multiple files are supplied

## OPTIONS

more accepts no flags.

## LIMITATIONS

- no backward navigation (use less for that)
- no search support
- no line-numbering option
- no `-N`, `-d`, or other display flags found in GNU/BSD more

## EXAMPLES

- `more file.txt` — page through a file
- `ls -l | more` — page through command output

## SEE ALSO

less, man, cat
