# HEXDUMP

## NAME

hexdump - display file contents in hexadecimal

## SYNOPSIS

```
hexdump [file ...]
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
- Prints a final line with the total byte offset

## OPTIONS

None. The current implementation outputs the canonical format only.

## LIMITATIONS

- No `-C` / `-x` / `-d` / `-o` format selection flags.
- No `-n LENGTH` (limit bytes), `-s OFFSET` (skip bytes), or `-v` (no
  squeezing identical lines).
- Output format is fixed at canonical hex+ASCII (equivalent to `hexdump -C`
  in many systems).

## EXAMPLES

```
hexdump file.bin
hexdump < /dev/urandom | head -4
dd if=file.bin bs=1 skip=0 count=64 | hexdump
```

## SEE ALSO

od, dd, strings
