# STAT

## NAME

stat - display file or file-system status

## SYNOPSIS

```
stat [-L] [-f] [-c FORMAT] PATH ...
```

## DESCRIPTION

`stat` displays metadata about each PATH: size, permissions, timestamps,
inode number, and link count. With `-f` it reports file-system statistics
instead of file metadata.

## CURRENT CAPABILITIES

- File metadata: name, size, blocks, type, mode, links, uid/gid, timestamps
- File-system metadata with `-f`: type, block size, total/free/available blocks
- Follow symlinks with `-L`
- Custom format string with `-c` (or `--format`/`--printf`)
- Format specifiers for file mode: `%n` (name), `%s` (size), `%b` (blocks),
  `%B` (block size), `%f` (hex mode), `%F` (type string), `%i` (inode),
  `%h` (links), `%u`/`%g` (uid/gid), `%U`/`%G` (user/group names),
  `%a` (octal perms), `%A` (permission string), `%x`/`%y`/`%z` (timestamps),
  `%X`/`%Y`/`%Z` (epoch seconds), `%W` (birth time), `%t`/`%T` (device)

## OPTIONS

- `-L`, `--dereference` — follow symlinks (stat the target, not the link)
- `-f`, `--file-system` — display file-system status instead of file status
- `-c FORMAT`, `--format=FORMAT` — use FORMAT instead of default output;
  appends newline after each path
- `--printf=FORMAT` — like `--format` but does not append a newline; escape
  sequences in FORMAT are interpreted

## LIMITATIONS

- The default (non-format) output layout may differ from GNU `stat`.
- No `--terse` mode.

## EXAMPLES

```
stat file.txt
stat -L /proc/self
stat -f /home
stat -c "%n: %s bytes" *.c
stat --printf="%Y\n" file.txt
```

## SEE ALSO

ls, file, du
