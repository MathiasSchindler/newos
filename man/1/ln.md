# LN

## NAME

ln - create hard links and symbolic links

## SYNOPSIS

```text
ln [-s] [-f] [-n] [-T] [-r] [-v] TARGET [LINK_NAME]
```

## DESCRIPTION

`ln` creates hard links by default and symbolic links with `-s`. It also
supports force, relative-link, and verbose modes for common filesystem tasks.

## CURRENT CAPABILITIES

- create hard links
- create symbolic links with `-s`
- force replacement with `-f`
- treat the destination literally with `-T`
- compute relative symbolic targets with `-r`
- verbose reporting with `-v`

## OPTIONS

- `-s` create a symbolic link instead of a hard link
- `-f` replace an existing destination when possible
- `-n` avoid dereferencing a symlink destination
- `-T` treat the link name as a normal path, not a directory target
- `-r` generate a relative symbolic target
- `-v` print the link that was created

## LIMITATIONS

- `-r` only applies to symbolic links
- platform filesystem semantics still determine what kinds of links are allowed

## EXAMPLES

```text
ln target.txt link.txt
ln -s ../bin/tool tool-link
```

## SEE ALSO

cp, mv, readlink
