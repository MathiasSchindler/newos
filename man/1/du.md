# DU

## NAME

du - estimate file space usage

## SYNOPSIS

```
du [-a] [-b] [-c] [-h] [-k] [-L|-P] [-B SIZE] [-d N] [-s] [path ...]
```

## DESCRIPTION

`du` reports the disk space used by each PATH (default: `.`). By default
it reports total sizes in bytes for each directory in the tree.

## CURRENT CAPABILITIES

- Report apparent sizes for all files with `-a`
- Use bytes (`-b`), kibibytes (`-k`), or human-readable sizes (`-h`)
- Arbitrary block size with `-B SIZE`
- Limit recursion depth with `-d N`
- Summarise only top-level with `-s`
- Grand total line with `-c`
- Follow symlinks with `-L`; do not follow with `-P` (default)

## OPTIONS

- `-a`, `--all` — report sizes for all files, not just directories
- `-b`, `--bytes` — use 1-byte blocks (show apparent size)
- `-c`, `--total` — append a grand total line
- `-h`, `--human-readable` — print sizes in human-readable form (K, M, G)
- `-k` — use 1024-byte blocks
- `-L`, `--dereference` — follow all symlinks
- `-P`, `--no-dereference` — do not follow symlinks (default)
- `-B SIZE`, `--block-size=SIZE` — use SIZE-byte blocks
- `-d N`, `--max-depth=N` — limit output to directories at most N levels deep
- `-s`, `--summarize` — report only a total for each argument

## LIMITATIONS

- No `--exclude` or `--exclude-from` patterns.
- No `-x` (one file system only).
- Hard-link accounting is simpler than GNU/BSD `du`; repeated links may not be
  de-duplicated in every traversal shape.
- Apparent-size, inode-count, threshold, and time-filter modes are not
  implemented.
- Block allocation and sparse-file reporting depend on what the platform
  backend exposes.

## EXAMPLES

```
du
du -sh /home/user
du -a -d 1 src/
du -k --total *.log
du -B 1M /var
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

df, stat, ls
