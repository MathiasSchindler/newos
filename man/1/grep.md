# GREP

## NAME

grep - search text for matching lines

## SYNOPSIS

```
grep [OPTIONS] PATTERN [FILE ...]
```

## DESCRIPTION

`grep` searches each FILE (or standard input) for lines matching PATTERN and
prints matching lines to standard output. PATTERN is a POSIX basic regular
expression by default; use `-E` for extended syntax.

## CURRENT CAPABILITIES

- BRE (basic) and ERE (extended with `-E`) regular expression matching
- Case-insensitive matching with `-i`
- Inverted matching with `-v`
- Recursive directory search with `-r`/`-R`
- Fixed-string matching with `-F`
- Whole-word matching with `-w`
- Print only matched portion with `-o`
- Before/after context lines with `-B`, `-A`, `-C`
- Line number output with `-n`
- Count-only output with `-c`
- Quiet mode (exit code only) with `-q`
- List matching file names with `-l`
- Multiple patterns with `-e PATTERN`
- optional colored match highlighting with `--color=WHEN`

## OPTIONS

- `-n` — prefix each matching line with its line number
- `-i` — case-insensitive matching
- `-v` — invert match; print non-matching lines
- `-r`, `-R` — recurse into directories
- `-c` — print a count of matching lines per file
- `-q` — quiet; suppress output, report match via exit code only
- `-l` — print only the names of files with at least one match
- `-F` — treat PATTERN as a fixed string, not a regex
- `-E` — use extended regular expressions (ERE)
- `-o` — print only the matched portion of each line
- `-w` — match whole words only
- `-A N` — print N lines of context after each match
- `-B N` — print N lines of context before each match
- `-C N` — print N lines of context before and after each match
- `-e PATTERN` — specify an additional pattern
- `--color[=WHEN]` — highlight matches and prefixes using `auto`, `always`, or `never`

## LIMITATIONS

- No support for Perl-compatible regular expressions (PCRE).
- No `-P` flag.
- Patterns are limited to 64 KiB, nesting is limited to 64 groups, and each search has a bounded matcher work budget to prevent pathological backtracking.
- Context separators are `--` (standard) but no `--group-separator` option.

Color output follows the shared project behavior documented in `output-style`.

## EXAMPLES

```
grep "error" log.txt
grep -i "warning" *.log
grep -rn "TODO" src/
grep -v "^#" config.txt
grep -E "^[0-9]+" data.txt
grep -o "[0-9]\+" file.txt
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

sed, awk, find, ripgrep, rg, output-style
