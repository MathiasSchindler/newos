# DIFF

## NAME

diff - compare files line by line

## SYNOPSIS

```
diff [-u|-c] [-q] [-r] file1 file2
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

## OPTIONS

- `-u` — produce a unified diff (context lines prefixed with `+`/`-`)
- `-c` — produce a context diff (surrounding context lines shown)
- `-q` — report only whether files differ, no line-level detail
- `-r` — recursively compare subdirectories

## LIMITATIONS

- No `-a` (treat binary files as text).
- No `--ignore-whitespace`, `--ignore-blank-lines`, or similar flags.
- No `-N` (treat absent files as empty) when doing recursive diffs.
- No patch-compatible output with `--label` or `--strip`.

## EXAMPLES

```
diff old.c new.c
diff -u original.txt modified.txt
diff -r src/ backup/src/
diff -q build1/ build2/
```

## SEE ALSO

cmp, patch
