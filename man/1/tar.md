# TAR

## NAME

tar - create or extract tar archives

## SYNOPSIS

tar [-v] [-C dir] -cf archive.tar paths... | tar [-v] [-C dir] -xf archive.tar | tar -tf archive.tar

## DESCRIPTION

The tar tool creates, extracts, and lists plain tar archives. It focuses on core archive workflows without built-in compression wrappers.

## CURRENT CAPABILITIES

- create tar archives from one or more paths
- extract archives into the current or chosen directory
- list archive contents without extracting them
- change directory before acting with `-C`
- print file names in verbose mode

## OPTIONS

| Flag | Description |
|------|-------------|
| `-c` | Create a new archive. |
| `-x` | Extract an existing archive. |
| `-t` | List archive contents. |
| `-f archive` | Use the specified archive file. |
| `-v` | Print file names as they are processed. |
| `-C dir` | Change to `dir` before operating. |

## LIMITATIONS

- No gzip, bzip2, or xz integration flags such as `-z`, `-j`, or `-J` are supported.
- POSIX extended headers, PAX format, and sparse file support are not implemented.
- Hard-link tracking is basic only.

## EXAMPLES

- `tar -cf backup.tar src docs`
- `tar -tf backup.tar`
- `tar -C out -xf backup.tar`

## SEE ALSO

gzip, gunzip, cp
