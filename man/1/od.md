# OD

## NAME

od - dump file contents in octal

## SYNOPSIS

```
od [file ...]
```

## DESCRIPTION

`od` displays the contents of each FILE (or standard input) as a sequence of
octal values. Output lines show the byte offset in octal followed by the octal
value of each byte in the 16-byte row.

## CURRENT CAPABILITIES

- Reads from standard input when no files are given
- Processes multiple files sequentially
- Outputs byte offset (7-digit octal) followed by per-byte octal values
- Prints a final line with the total byte count

## OPTIONS

None. The current implementation does not accept format flags.

## LIMITATIONS

- No `-t` (type string for output format: `x`, `d`, `u`, `c`, etc.).
- No `-A` (address base selection).
- No `-j` (skip bytes), `-N` (limit bytes), `-v` (no squeezing), `-w`
  (words per line) options.
- Output is always octal bytes, 16 per line; no multi-byte word grouping.

## EXAMPLES

```
od file.bin
od < /dev/urandom | head -5
echo "hello" | od
```

## SEE ALSO

hexdump, dd
