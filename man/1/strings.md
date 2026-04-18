# STRINGS

## NAME

strings - print printable character sequences from a file

## SYNOPSIS

```
strings [-n MIN] [-o] [-f] [-t RADIX] [-e ENCODING] [file ...]
```

## DESCRIPTION

`strings` scans each FILE (or standard input) and prints sequences of
printable characters that are at least MIN bytes long (default: 4).

## CURRENT CAPABILITIES

- Configurable minimum string length with `-n`
- Print byte offsets with `-o` (octal) or `-t` (decimal/octal/hex)
- Print the file name before each string with `-f`
- Encoding selection: 8-bit, 7-bit, UTF-16 LE/BE, UTF-32 LE/BE with `-e`

## OPTIONS

- `-n MIN`, `-MIN` — minimum string length (default: 4)
- `-o` — print octal offset before each string
- `-f` — print file name before each string
- `-t d|o|x` — print offset in decimal, octal, or hexadecimal
- `-e ENCODING` — character encoding: `s` (7-bit), `S` (8-bit, default),
  `l` (16-bit LE), `b` (16-bit BE), `L` (32-bit LE), `B` (32-bit BE)

## LIMITATIONS

- No ELF section filtering (always scans the full file byte-by-byte).
- No `-a` / `--all` option (the entire file is always scanned).

## EXAMPLES

```
strings binary
strings -n 8 library.so
strings -t x firmware.bin
strings -f *.o | grep "version"
strings -e l wide_chars.bin
```

## SEE ALSO

hexdump, od, file
