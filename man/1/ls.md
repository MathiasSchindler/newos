# LS

## NAME

ls - list files and directories

## SYNOPSIS

ls [OPTIONS] [PATH ...]

## DESCRIPTION

The ls tool lists directory entries from one or more paths. It supports the core project workflows for browsing files, inspecting metadata, and working comfortably inside the hosted environment.

## CURRENT CAPABILITIES

- plain directory listings
- long output mode with metadata
- show hidden entries with `-a` or `-A`
- sort by name, size, or modification time
- recursive traversal and directory-only views
- inode, block, numeric owner, and classification output

## OPTIONS

| Flag | Description |
|------|-------------|
| `-l` | Use long format. |
| `-a` | Include dotfiles, including `.` and `..`. |
| `-A` | Include dotfiles except `.` and `..`. |
| `-1` | Show one entry per line. |
| `-h` | Use human-readable sizes in long mode. |
| `-r` | Reverse the sort order. |
| `-S` | Sort by file size. |
| `-t` | Sort by modification time. |
| `-R` | Recurse into subdirectories. |
| `-d` | List directory names themselves, not their contents. |
| `-n` | Show numeric UID and GID in long mode. |
| `-F` | Classify entries with trailing type markers. |
| `-i` | Show inode numbers. |
| `-s` | Show block usage. |
| `-p` | Append `/` to directory names. |
| `-G` | Suppress group output in long format. |
| `-q` | Replace nonprinting characters with `?`. |

## LIMITATIONS

- No colorized output is implemented.
- Extended attributes are not displayed.
- Column widths are best-effort rather than exact GNU/BSD matching.

## EXAMPLES

- `ls`
- `ls -l`
- `ls -la src`

## SEE ALSO

cp, pwd, find, stat
