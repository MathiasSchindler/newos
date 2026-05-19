# DIFF

## NAME

diff - compare files line by line

## SYNOPSIS

```
diff [-u|-c] [-q] [-r] [-w] [-b] [-B] [-i] [--color[=WHEN]] file1 file2
```

## DESCRIPTION

`diff` compares FILE1 and FILE2 line by line and outputs the differences.
Exits 0 when files are identical, 1 when they differ, and 2 on error.

## CURRENT CAPABILITIES

- Default output format showing changed line ranges
- Unified diff format with `-u`
- Context diff format with `-c`
- Brief (files-differ only) mode with `-q`
- Recursive directory comparison with `-r`
- Ignore whitespace-only differences with `-w` / `-b`
- Ignore blank lines with `-B`
- Ignore ASCII case differences with `-i`
- optional colored headers and additions/removals with `--color=WHEN`

## OPTIONS

- `-u` — produce a unified diff (context lines prefixed with `+`/`-`)
- `-c` — produce a context diff (surrounding context lines shown)
- `-q` — report only whether files differ, no line-level detail
- `-r` — recursively compare subdirectories
- `-w` — ignore all horizontal whitespace differences
- `-b` — ignore changes in the amount of whitespace
- `-B` — ignore blank lines
- `-i` — ignore ASCII letter case changes
- `--color[=WHEN]` — colorize diff headers and changed lines using `auto`, `always`, or `never`

## LIMITATIONS

- No `-a` (treat binary files as text).
- No `-N` (treat absent files as empty) when doing recursive diffs.
- No patch-compatible output with `--label` or `--strip`.

Color output follows the shared project behavior documented in `output-style`.

## EXAMPLES

```
diff old.c new.c
diff -u original.txt modified.txt
diff -w -B config.old config.new
diff -r src/ backup/src/
diff -q build1/ build2/
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

cmp, patch, output-style
