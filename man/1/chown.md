# CHOWN

## NAME

chown - change file ownership

## SYNOPSIS

```text
chown [-R] [-h] [--reference=FILE | OWNER[:GROUP]] PATH...
```

## DESCRIPTION

`chown` changes the owner and optionally the group of one or more paths. It can
also copy ownership from a reference file.

## CURRENT CAPABILITIES

- set owner and group numerically or by name
- recursive ownership changes with `-R`
- do-not-follow behavior for symlinks with `-h`
- ownership copying from `--reference=FILE`

## OPTIONS

- `-R` recurse through directory trees
- `-h` operate on symlink entries instead of their targets where supported
- `--reference=FILE` copy owner and group from another path

## LIMITATIONS

- ownership changes depend on the host or target platform permissions
- advanced ACL or extended attribute behavior is outside the current scope

## EXAMPLES

```text
chown root:root path
chown -R builduser build/
chown --reference=template.txt target.txt
```

## SEE ALSO

chmod, stat, id
