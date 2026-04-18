# DF

## NAME

df - report file system disk space usage

## SYNOPSIS

```
df [-h] [-i] [-k] [-T] [path ...]
```

## DESCRIPTION

`df` displays available and used disk space for each file system. Without
arguments it reports all mounted file systems. With PATH arguments it reports
the file system containing each PATH.

## CURRENT CAPABILITIES

- List all mounted file systems or those containing specified paths
- Human-readable sizes with `-h`
- Inode statistics with `-i`
- 1024-byte block units with `-k`
- Show filesystem type with `-T`

## OPTIONS

- `-h` — print sizes in human-readable form (K, M, G, T)
- `-i` — show inode usage instead of block usage
- `-k` — use 1024-byte blocks
- `-T` — print the filesystem type

## LIMITATIONS

- No `-a` (include pseudo/virtual file systems).
- No `-x TYPE` (exclude a filesystem type).
- No `--output` column selection.
- Block size defaults to 1 byte (not 1 K) unless `-k` is given.

## EXAMPLES

```
df
df -h
df -i /home
df -k -T /
```

## SEE ALSO

du, stat, mount
