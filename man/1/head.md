# HEAD

## NAME

head - output the first part of files

## SYNOPSIS

```
head [-n [+|-]COUNT | -c [+|-]COUNT] [-qv] [file ...]
```

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
- No quiet/verbose header-control long options beyond the documented flags.
- Negative counts such as GNU `head -n -5` are not implemented.
- Very long lines are streamed for normal output, but some byte-count and
  multi-file edge cases remain simpler than GNU/BSD behavior.

## EXAMPLES

```
head file.txt
head -n 20 log.txt
head -c 128 data.bin
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

tail, cat, wc
