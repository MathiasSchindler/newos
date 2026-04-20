# TAR

## NAME

tar - create or extract tar archives

## SYNOPSIS

tar [-v] [-P] [-C dir] [--exclude PATTERN] -cf archive.tar paths...

tar [-v] [-P] [-C dir] [--strip-components N] -xf archive.tar [members...]

tar [-v] [--strip-components N] -tf archive.tar [members...]

## DESCRIPTION

The tar tool creates, extracts, and lists `ustar` archives with a practical
subset of familiar workflow features. It keeps the implementation self-contained
and works with the in-tree `gzip`, `bzip2`, and `xz` helpers rather than adding
external runtime dependencies.

## CURRENT CAPABILITIES

- create archives from one or more paths
- extract or list only selected members by exact name or wildcard pattern
- skip matching paths during create/list/extract with `--exclude`
- strip leading path components during list or extract with `--strip-components`
- handle longer paths using `ustar` prefixes plus GNU long-name records when needed
- change directory before acting with `-C`
- print file names in verbose mode
- use `-z`, `-j`, and `-J` for gzip, bzip2, and xz archive workflows
- preserve symbolic links and restore hard links from compatible archives
- read common GNU/PAX path metadata from archives produced by host tar tools
- refuse unsafe extraction paths such as absolute names or `..` traversal unless
  `-P` / `--absolute-names` is given

## OPTIONS

| Flag | Description |
|------|-------------|
| `-c` | Create a new archive. |
| `-x` | Extract an existing archive. |
| `-t` | List archive contents. |
| `-f archive` | Use the specified archive file. |
| `-v` | Print file names as they are processed. |
| `-C dir` | Change to `dir` before operating. |
| `-z` | Use gzip compression/decompression helpers. |
| `-j` | Use bzip2 compression/decompression helpers. |
| `-J` | Use xz compression/decompression helpers. |
| `--exclude pattern` | Skip paths or members matching the wildcard pattern. |
| `--strip-components N` | Remove the first `N` path components when listing or extracting. |
| `-P`, `--absolute-names` | Allow absolute or parent-traversing member paths during extract. |

## LIMITATIONS

- No append/update/delete modes (`-r`, `-u`, `--delete`) are supported.
- Sparse files and device/special-file recreation are still not implemented.
- Only the most common GNU/PAX metadata fields are honored; richer ownership or
  extended-attribute preservation is intentionally minimal.
- Compressed streaming to or from standard input/output is still limited compared
  with a full GNU tar implementation.

## EXAMPLES

- `tar -cf backup.tar src docs`
- `tar -czf backup.tar.gz src docs`
- `tar -tf backup.tar 'src/*.c'`
- `tar --exclude='build/*' -cf source.tar src build`
- `tar -C out --strip-components=1 -xf backup.tar docs/readme.txt`

## SEE ALSO

gzip, gunzip, cp
