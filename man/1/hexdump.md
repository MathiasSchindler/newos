# HEXDUMP

## NAME

hexdump - display file contents in hexadecimal

## SYNOPSIS

```
hexdump [-C | -x | -d | -o] [-A BASE] [-v] [-s OFFSET] [-n LENGTH] [file ...]
```

## DESCRIPTION

`hexdump` displays the contents of each FILE (or standard input). The default
is the canonical hex+ASCII dump: each output line shows an 8-digit hexadecimal
offset, 16 bytes of hexadecimal values, and a printable ASCII representation
with non-printable bytes replaced by `.`. Alternate 16-bit word views are also
available.

## CURRENT CAPABILITIES

- Reads from standard input when no files are given
- Processes multiple files sequentially
- Canonical format: offset, 16 hex bytes, ASCII panel
- Alternate 16-bit little-endian word formats with `-x`, `-d`, and `-o`
- Address-base selection with `-A x`, `-A d`, `-A o`, and `-A n`
- `-s OFFSET` skips bytes before dumping
- `-n LENGTH` limits the number of bytes dumped
- `-C` selects the canonical format explicitly; `-v` is accepted for
  compatibility because this implementation never squeezes duplicate lines
- Prints a final line with the total byte offset

## OPTIONS

| Flag | Description |
|------|-------------|
| `-C` | Use canonical hex+ASCII output. This is also the default. |
| `-x` | Display 16-bit little-endian words in hexadecimal. |
| `-d` | Display 16-bit little-endian words in unsigned decimal. |
| `-o` | Display 16-bit little-endian words in octal. |
| `-A BASE` | Select the address base: `x` hex, `d` decimal, `o` octal, or `n` none. |
| `-s OFFSET` | Skip OFFSET bytes before dumping. |
| `-n LENGTH` | Dump at most LENGTH bytes. |
| `-v` | Accepted for compatibility; duplicate lines are always shown. |

## LIMITATIONS

- No custom format strings (`-e`, `-f`) are implemented.
- Alternate numeric formats are limited to 16-bit little-endian words.
- Duplicate-line squeezing is intentionally absent; `-v` is accepted as a no-op.

## EXAMPLES

```
hexdump file.bin
hexdump -C -s 128 -n 64 file.bin
hexdump -x file.bin
hexdump -d -A d file.bin
hexdump < /dev/urandom | head -4
dd if=file.bin bs=1 skip=0 count=64 | hexdump
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

od, dd, strings
