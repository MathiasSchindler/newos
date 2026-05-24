# STRINGS

## NAME

strings - print printable character sequences from a file

## SYNOPSIS

```
strings [-a|-d] [-n MIN] [-o] [-f] [-t RADIX] [-e ENCODING] [file ...]
```

## DESCRIPTION

`strings` scans each FILE (or standard input) and prints sequences of
printable characters that are at least MIN bytes long (default: 4).

By default `strings` scans the entire byte stream. With `-d`, recognised object
files are scanned through their initialized section ranges instead, which skips
headers, section tables, code signatures, and uninitialized space where possible.
If a file is not a recognised object format, `-d` falls back to the full byte
stream.

## CURRENT CAPABILITIES

- Configurable minimum string length with `-n`
- Full-file scanning with `-a` / `--all` and object section scanning with `-d` /
  `--data`
- Print byte offsets with `-o` (octal) or `-t` (decimal/octal/hex)
- Print the file name before each string with `-f`
- Encoding selection: 8-bit, 7-bit, UTF-16 LE/BE, UTF-32 LE/BE with `-e`
- Section-aware scanning for ELF64 little-endian, Mach-O 64-bit little-endian,
  and PE/COFF executable images

## OPTIONS

- `-a`, `--all` — scan the entire file (default)
- `-d`, `--data` — scan initialized object-file sections for recognised ELF,
  Mach-O, and PE/COFF inputs
- `-n MIN`, `-MIN` — minimum string length (default: 4)
- `-o` — print octal offset before each string
- `-f` — print file name before each string
- `-t d|o|x` — print offset in decimal, octal, or hexadecimal
- `-e ENCODING` — character encoding: `s` (7-bit), `S` (8-bit, default),
  `l` (16-bit LE), `b` (16-bit BE), `L` (32-bit LE), `B` (32-bit BE)

## LIMITATIONS

- Object-aware scanning is intentionally shallow: ELF support is limited to
  64-bit little-endian section headers, Mach-O support to 64-bit little-endian
  `LC_SEGMENT_64` sections, and PE/COFF support to standard image section
  tables.
- Mach-O zero-fill sections are skipped, and section ranges are checked against
  the file size before scanning.
- Archive member traversal, compressed-file expansion, relocation awareness, and
  debug metadata interpretation are not implemented.
- Unicode decoding is limited to the documented byte-oriented ASCII-compatible
  encodings; there is no UTF-8 validation or automatic encoding detection.

## EXAMPLES

```
strings binary
strings -d app.exe
strings -d build/newlinker-macos-aarch64/echo
strings -n 8 library.so
strings -t x firmware.bin
strings -f *.o | grep "version"
strings -e l wide_chars.bin
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

hexdump, od, file
