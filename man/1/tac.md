# TAC

## NAME

tac - concatenate files in reverse

## SYNOPSIS

```
tac [-s SEP|-0] [file ...]
```

## DESCRIPTION

tac reads one or more files (or standard input) and writes their contents with the order of records reversed. By default records are newline-delimited lines.

## CURRENT CAPABILITIES

- reversing lines of files and standard input
- custom record separator via `-s`
- NUL-delimited record mode via `-0`
- processing multiple files in sequence

## OPTIONS

- `-s SEP` — use SEP as the record separator instead of newline
- `-0` — use NUL as the record separator

## LIMITATIONS

- the entire file content must fit within the internal buffer; very large files may fail
- no `-r` (treat separator as a regex) as in GNU tac

## EXAMPLES

- `tac file.txt` — print lines in reverse order
- `tac -s $'\n---\n' doc.txt` — reverse multi-line records separated by `---`
- `tac -0 nul-file` — reverse NUL-separated records

## SEE ALSO

cat, rev, sort
