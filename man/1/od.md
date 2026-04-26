# OD

## NAME

od - dump file contents in octal

## SYNOPSIS

```
od [-A d|o|x|n] [-t x1|o1|d1|u1|c] [-j SKIP] [-N COUNT] [-w WIDTH] [file ...]
```

## DESCRIPTION

`od` displays the contents of each FILE (or standard input) as a sequence of
octal values. Output lines show the byte offset in octal followed by the octal
value of each byte in the 16-byte row.

## CURRENT CAPABILITIES

- Reads from standard input when no files are given
- Processes multiple files sequentially
- Outputs byte offsets in octal, decimal, hexadecimal, or no address column
- Supports octal, hexadecimal, signed/unsigned decimal byte, and character views
- Can skip an initial byte count, limit the number of bytes dumped, and adjust
  bytes per output row
- Prints a final line with the total byte count

## OPTIONS

| Flag | Description |
|------|-------------|
| `-A d|o|x|n` | Select decimal, octal, hexadecimal, or omitted addresses. |
| `-t x1|o1|d1|u1|c` | Select one-byte hex, octal, decimal, unsigned decimal, or character output. |
| `-j SKIP` | Skip SKIP bytes before dumping. |
| `-N COUNT` | Dump at most COUNT bytes. |
| `-w WIDTH` | Emit WIDTH bytes per output row. |

## LIMITATIONS

- Type strings are limited to one-byte forms (`x1`, `o1`, `d1`, `u1`, `c`); multi-byte word grouping and endian-aware numeric formats are not implemented.
- No duplicate-line squeezing is performed, so `-v` is unnecessary and not accepted.

## EXAMPLES

```
od file.bin
od -A x -t x1 -j 16 -N 32 file.bin
od -A n -t c file.txt
od < /dev/urandom | head -5
echo "hello" | od
```

## SEE ALSO

hexdump, dd
