# HEAD

## NAME

head - output the first part of files

## SYNOPSIS

head [-n [+|-]COUNT | -c [+|-]COUNT] [-qv] [file ...]

## DESCRIPTION

The head tool prints the beginning of files by line count or byte count. It supports the common `+N` and `-N` variants for alternate ranges.

## CURRENT CAPABILITIES

- print the first 10 lines by default
- select by lines or bytes
- start from a given line or byte with `+N` syntax
- omit or force headers for multi-file output

## OPTIONS

| Flag | Description |
|------|-------------|
| `-n [+\|-]COUNT` | Select lines: first `COUNT`, from line `+N`, or all but the last `N`. |
| `-c [+\|-]COUNT` | Select bytes with the same forms as `-n`. |
| `-q` | Suppress file headers. |
| `-v` | Always print file headers. |

## LIMITATIONS

- No NUL-delimited `-z` mode is implemented.
- The internal buffer size is finite.

## EXAMPLES

- `head file.txt`
- `head -n 20 log.txt`
- `head -c 128 data.bin`

## SEE ALSO

tail, cat, wc
