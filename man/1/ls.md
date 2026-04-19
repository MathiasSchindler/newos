# LS

## NAME

ls - list files and directories

## SYNOPSIS

ls [OPTIONS] [PATH ...]

## DESCRIPTION

The ls tool lists directory entries from one or more paths. It supports the core project workflows for browsing files, inspecting metadata, and working comfortably inside the hosted environment.

## CURRENT CAPABILITIES

- plain directory listings
- long output mode with metadata, readable local timestamps, and symlink targets
- show hidden entries with `-a` or `-A`
- sort by name, size, or modification time
- recursive traversal and directory-only views
- inode, block, numeric owner, classification, and optional color output
- shared `--color=auto|always|never` behavior for tty-friendly listings

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
| `--full-time` | Show timestamps with seconds. |
| `--time-style=STYLE` | Use `long-iso`, `full-iso`, or `unix` timestamps. |
| `--color[=WHEN]` | Use colored output (`auto`, `always`, `never`). |

## LIMITATIONS

- Extended attributes are not displayed.
- Output is intentionally simpler than GNU/BSD `ls`; multi-column terminal layout
  and some edge-case compatibility flags are not implemented.
- Column widths are best-effort rather than exact GNU/BSD matching.

Color output follows the shared project behavior documented in `output-style`.

## EXAMPLES

- `ls`
- `ls -l`
- `ls -la src`

## SEE ALSO

cp, pwd, find, stat, output-style
