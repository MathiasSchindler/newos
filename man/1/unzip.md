# UNZIP

## NAME

unzip - list, test, and extract ZIP archives

## SYNOPSIS

```
unzip [-l|-t|-p] [-d DIR] ARCHIVE [ENTRY ...]
```

## DESCRIPTION

`unzip` reads ZIP archives and lists, tests, extracts, or writes selected entries.
Entry arguments may be exact names or simple wildcard patterns.

Extraction refuses absolute paths, backslash paths, drive-letter paths, and
`..` components so archives cannot write outside the destination tree.

## OPTIONS

- `-l` - list matching entries as size, method, and name.
- `-t` - test matching entries by reading and verifying their payloads.
- `-p` - write matching file payloads to standard output.
- `-d DIR` - extract into DIR instead of the current directory.
- `-h`, `--help` - show usage.

## LIMITATIONS

This first implementation supports stored and deflated entries through the
project ZIP reader. Encrypted ZIPs, split archives, symlink restoration, comments,
and external attributes are not interpreted yet.

## SEE ALSO

zip, tar, readapk, gzip, gunzip
