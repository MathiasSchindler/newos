# CHOWN

## NAME

chown - change file ownership

## SYNOPSIS

```
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
- Recursive traversal does not provide `--preserve-root`, filesystem-boundary
  controls, or the full GNU symlink traversal matrix.
- Name lookup depends on the platform identity backend; alternate passwd/group
  files, NSS modules, and directory-service integrations are not implemented.
- SELinux contexts, file capabilities, and other security labels are not
  changed.

## EXAMPLES

```
chown root:root path
chown -R builduser build/
chown --reference=template.txt target.txt
```

## SEE ALSO

chmod, stat, id
