# LN

## NAME

ln - create hard links and symbolic links

## SYNOPSIS

```
ln [OPTION ...] TARGET [LINK_NAME]
ln [OPTION ...] TARGET ... DIRECTORY
```

## DESCRIPTION

`ln` creates hard links by default and symbolic links with `-s`. If only
`TARGET` is given, the link is created in the current directory using the
target's basename. With multiple source operands, the final operand must be an
existing directory.

## CURRENT CAPABILITIES

- create hard links
- create symbolic links with `-s`/`--symbolic`
- force replacement of an existing non-directory destination with `-f`
- treat the destination literally with `-T`
- compute relative symbolic targets with `-r`
- link multiple sources into an existing directory
- verbose reporting with `-v`

## OPTIONS

- `-s`, `--symbolic` — create a symbolic link instead of a hard link
- `-f`, `--force` — replace an existing destination when possible
- `-n`, `--no-dereference` — avoid treating a symlink destination as a directory
- `-T`, `--no-target-directory` — treat the link name as a normal path, not a
  directory target
- `-r`, `--relative` — generate a relative symbolic target
- `-v`, `--verbose` — print the link that was created

## LIMITATIONS

- `-r`/`--relative` only applies to symbolic links and fails without `-s`
- `-f` refuses to replace an existing directory
- platform filesystem semantics and permissions still determine what kinds of links are allowed

## EXAMPLES

```
ln file.txt
ln target.txt link.txt
ln -s ../bin/tool tool-link
ln -svr /opt/newos/bin/tool ./tool-link
```

## SEE ALSO

cp, mv, readlink
