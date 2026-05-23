# CPIO

## NAME

cpio - create, list, and extract newc archives

## SYNOPSIS

```
cpio -o [-F ARCHIVE] FILE ...
cpio -t [-F ARCHIVE] [--json]
cpio -i [-F ARCHIVE]
```

## DESCRIPTION

`cpio` handles the portable ASCII `newc` archive format. It can create archives
from named files, list archive contents, and extract archive members.

When `-F` is omitted, create mode writes to standard output and list/extract mode
reads from standard input.

## OPTIONS

- `-o` - create an archive.
- `-t` - list archive entries.
- `-i` - extract archive entries.
- `-F ARCHIVE` - read or write ARCHIVE instead of standard input/output.
- `--json` - in list mode, emit JSON Lines `entry` events instead of names.
- `-h`, `--help` - show usage.

## JSON Output

With `--json` and `-t`, `cpio` emits one `entry` event per archive member:

```json
{"schema":"newos.tool.v1","tool":"cpio","stream":"stdout","event":"entry","seq":1,"data":{"name":"file.txt","size":12,"mode":33188}}
```

Diagnostics and usage messages follow the shared `json-output` envelope.

## LIMITATIONS

The first implementation supports regular files and directories in `newc`
archives. Recursive creation, pattern selection, device nodes, symlink payloads,
ownership restoration, and compression are not implemented yet.

## SEE ALSO

tar, ar, gzip
