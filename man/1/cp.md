# CP

## NAME

cp - copy files and directory trees

## SYNOPSIS

```
cp [OPTIONS] SOURCE DEST
cp [OPTIONS] SOURCE ... DIRECTORY
```

## DESCRIPTION

The cp tool copies regular files and, when requested, entire directory trees. In this project it is intended to provide practical copy behavior without external dependencies.

## CURRENT CAPABILITIES

- copy one file to another
- copy multiple files into a directory
- copy directory trees recursively
- preserve timestamps and permissions for common cases
- support archive, update, and link-oriented modes
- control how symlinks are treated during copies

## OPTIONS

| Flag | Description |
|------|-------------|
| `-r`, `-R` | Copy directories recursively. |
| `-f` | Overwrite existing destinations. |
| `-i` | Prompt before overwriting. |
| `-n` | Do not overwrite existing files. |
| `-v` | Print each copy action. |
| `-a` | Archive mode: preserve metadata and recurse. |
| `-p` | Preserve timestamps and permissions. |
| `-u` | Skip files when the destination is newer. |
| `-l` | Create hard links instead of copying file data. |
| `-s` | Create symbolic links instead of copying file data. |
| `-T` | Treat the destination as a non-directory path. |
| `-H`, `-L`, `-P` | Control how symlinks are followed. |

## LIMITATIONS

- ACLs, xattrs, and other extended metadata are not preserved.
- No copy-on-write or reflink optimization is implemented.
- Sparse-file detection, clone-on-write preservation, and filesystem-specific
  copy accelerators are not implemented yet.
- Backup suffixes, `--parents`, `--one-file-system`, and SELinux context
  handling are outside the current option surface.
- Special-file behavior is intentionally conservative; device nodes, FIFOs, and
  sockets may not be recreated with full `cp -a` fidelity.

## EXAMPLES

```
cp notes.txt notes.bak
cp -r src backup
cp -a tree dest
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

mv, ln, rm, tar
