# MV

## NAME

mv - move or rename files and directories

## SYNOPSIS

```text
mv [-i] [-f] [-n] [-u] [-v] source... destination
```

## DESCRIPTION

`mv` renames paths or moves them into a destination directory. It supports
common interactive, no-clobber, update-only, and verbose behaviors.

## CURRENT CAPABILITIES

- rename a single file or directory
- move multiple sources into an existing directory
- prompt before overwrite with `-i`
- skip overwrite with `-n`
- update only when the source is newer with `-u`
- verbose reporting with `-v`

## OPTIONS

- `-i` prompt before replacing a destination
- `-f` prefer forced overwrite behavior
- `-n` do not overwrite existing targets
- `-u` move only when it would update the destination
- `-v` print each move

## LIMITATIONS

- semantics are focused on common project usage rather than exhaustive GNU compatibility
- some metadata-preservation behavior can vary by platform backend

## EXAMPLES

```text
mv old.txt new.txt
mv -v src1 src2 destdir/
```

## SEE ALSO

cp, rm, ln
