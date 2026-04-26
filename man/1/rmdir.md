# RMDIR

## NAME

rmdir - remove empty directories

## SYNOPSIS

```
rmdir [-p] [-v] [--ignore-fail-on-non-empty] directory ...
```

## DESCRIPTION

`rmdir` removes empty directories. It can also walk upward and remove empty
parents when requested.

## CURRENT CAPABILITIES

- remove empty directories
- remove empty parent chains with `-p`
- verbose output with `-v`
- optionally ignore non-empty targets

## OPTIONS

- `-p` remove parent directories when they become empty
- `-v` print each removed directory
- `--ignore-fail-on-non-empty` suppress failure for non-empty paths

## LIMITATIONS

- it is specifically for empty-directory removal; use `rm -r` for trees
- behavior is intentionally focused on the core Unix workflow

## EXAMPLES

```
rmdir empty-dir
rmdir -p a/b/c
```

## SEE ALSO

rm, mkdir
