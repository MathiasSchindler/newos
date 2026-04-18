# CSPLIT

## NAME

csplit - split a file based on context (patterns)

## SYNOPSIS

csplit [-f PREFIX] [-n DIGITS] [-s|-q] [-k] [-z] file PATTERN...

## DESCRIPTION

csplit splits a file into sections defined by line-number or regex PATTERN arguments, writing each section to a separate output file. Unlike split, boundaries are determined by content rather than size.

## CURRENT CAPABILITIES

- splitting on exact line numbers
- splitting on `/regex/` patterns (start of matching line) with optional offset
- splitting on `%regex%` patterns (skip to match without writing preceding lines)
- repetition of a pattern with `{N}` or `{*}` suffix
- configurable output filename prefix and digit count
- keep-on-error mode to preserve partial output
- elide-empty-files mode

## OPTIONS

- `-f PREFIX` — use PREFIX for output filenames (default `xx`)
- `-n DIGITS` — use DIGITS-wide numeric suffix in output names (default 2)
- `-s` / `-q` / `--quiet` / `--silent` — suppress printing byte counts for each output file
- `-k` — keep output files on error instead of deleting them
- `-z` / `--elide-empty-files` — remove empty output files

## LIMITATIONS

- the entire file is loaded into memory; very large files may fail
- regex support uses the internal lightweight engine; complex POSIX ERE patterns may not be fully supported
- no `--suffix-format` option

## EXAMPLES

- `csplit file.txt 10 20` — split before lines 10 and 20
- `csplit file.txt /^Chapter/ {*}` — split before every line starting with `Chapter`
- `csplit -f ch_ -n 3 book.txt '/^#/' '{*}'` — produce `ch_001`, `ch_002`, …

## SEE ALSO

split, grep, sed, awk
