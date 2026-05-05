# CHGRP

## NAME

chgrp - change file group ownership

## SYNOPSIS

```
chgrp [-R] [-h] [--reference=FILE | GROUP] PATH...
```

## DESCRIPTION

`chgrp` changes the group owner of one or more paths. Groups may be named or numeric, and ownership can also be copied from a reference path.

## CURRENT CAPABILITIES

- set group ownership by name or numeric GID
- recursively change directory trees with `-R`
- operate on symlink entries with `-h` where supported
- copy the group from another path with `--reference=FILE`

## OPTIONS

- `-R` recurse through directory trees
- `-h` operate on symlink entries instead of their targets where supported
- `--reference=FILE` copy the group from FILE

## LIMITATIONS

- changes depend on platform ownership permissions
- group lookup depends on the platform identity backend
- recursive traversal does not implement filesystem-boundary or preserve-root controls
- ACLs, extended attributes, and security labels are outside the current scope

## EXAMPLES

```
chgrp staff notes.txt
chgrp -R builders obj/
chgrp --reference=template.txt output.txt
```

## SEE ALSO

chown, chmod, stat, id
