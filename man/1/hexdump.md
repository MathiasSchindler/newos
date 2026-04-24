# HEXDUMP

## NAME

hexdump - display file contents in hexadecimal

## SYNOPSIS

```
hexdump [-C] [-v] [-s OFFSET] [-n LENGTH] [file ...]
```

## DESCRIPTION

`hexdump` displays the contents of each FILE (or standard input) as a
canonical hex+ASCII dump. Each output line shows an 8-digit hexadecimal offset,
16 bytes of hexadecimal values, and a printable ASCII representation with
non-printable bytes replaced by `.`.

## CURRENT CAPABILITIES

- Reads from standard input when no files are given
- Processes multiple files sequentially
- Canonical format: offset, 16 hex bytes, ASCII panel
- `-s OFFSET` skips bytes before dumping
- `-n LENGTH` limits the number of bytes dumped
- `-C` selects the canonical format explicitly; `-v` is accepted for
  compatibility because this implementation never squeezes duplicate lines
- Prints a final line with the total byte offset

## OPTIONS

| Flag | Description |
|------|-------------|
| `-C` | Use canonical hex+ASCII output. This is also the default. |
| `-s OFFSET` | Skip OFFSET bytes before dumping. |
| `-n LENGTH` | Dump at most LENGTH bytes. |
| `-v` | Accepted for compatibility; duplicate lines are always shown. |

## LIMITATIONS

- No `-x` / `-d` / `-o` alternate format selection flags.
- Output format is fixed at canonical hex+ASCII (equivalent to `hexdump -C`
  in many systems).

## EXAMPLES

```
hexdump file.bin
hexdump -C -s 128 -n 64 file.bin
hexdump < /dev/urandom | head -4
dd if=file.bin bs=1 skip=0 count=64 | hexdump
```

## SEE ALSO

od, dd, strings
