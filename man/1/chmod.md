# CHMOD

## NAME

chmod - change file mode bits

## SYNOPSIS

```
chmod [-R] [-h] [-H|-L|-P] MODE path ...
```

## DESCRIPTION

`chmod` changes permissions on files and directories. It supports recursive
operation and both numeric and symbolic mode specifications.

## CURRENT CAPABILITIES

- numeric/octal mode changes
- symbolic mode updates for read, write, and execute bits
- recursive traversal with `-R`
- symlink-handling controls with `-h`, `-H`, `-L`, and `-P`

## OPTIONS

- `-R` recurse through directory trees
- `-h` operate on symlinks without dereferencing where supported
- `-H`, `-L`, `-P` control symlink traversal behavior
- `MODE` may be numeric or symbolic

## LIMITATIONS

- the command focuses on standard permission bits rather than every extended host feature
- support for symlink mode changes depends on platform behavior
- ACLs, file flags, SELinux labels, capabilities, and extended attributes are
  not modified.
- Symbolic modes cover the common `ugo+-=rwxX` style forms but not every GNU
  extension or host-specific permission bit.
- Recursive traversal does not provide `--preserve-root`, `--reference`, or
  filesystem-boundary controls.

## EXAMPLES

```
chmod 755 script.sh
chmod -R u+rwX,go-rwx private-dir
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

chown, stat, ls
