# TRUNCATE

## NAME

truncate - shrink or extend the size of files

## SYNOPSIS

```
truncate [-c] [-o] -s SIZE file...
```

## DESCRIPTION

truncate sets the length of each file to SIZE bytes. If a file is shorter it is extended with zero bytes; if longer it is truncated. The `-s` option is required.

## CURRENT CAPABILITIES

- setting an absolute file size
- extending files with zero-fill
- truncating files to a smaller size
- skip-create mode to ignore non-existent files
- size specifications with suffix multipliers and relative adjustments

## OPTIONS

- `-c` / `--no-create` — do not create files that do not already exist
- `-o` / `--io-blocks` — treat SIZE as a number of I/O blocks rather than bytes
- `-s SIZE` — target size; SIZE may include:
  - unit suffixes: `K` (KiB), `M` (MiB), `G` (GiB), `T` (TiB)
  - relative prefix `+` (increase by) or `-` (decrease by)
  - `<` (shrink to at most) or `>` (extend to at least)
  - `%N` (round down to a multiple of N) or `/N` (same)

## LIMITATIONS

- no `--reference` option to copy size from another file

## EXAMPLES

- `truncate -s 0 file.txt` — empty a file without removing it
- `truncate -s 1M image.bin` — extend or truncate to exactly 1 MiB
- `truncate -s +4K file` — increase size by 4 KiB
- `truncate -c -s 0 maybe.txt` — zero if exists, skip if not

## SEE ALSO

dd, fallocate, sync
