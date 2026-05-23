# TREE

## NAME

tree - display a directory tree

## SYNOPSIS

```
tree [-a] [-d] [-L LEVEL] [--json] [PATH ...]
```

## DESCRIPTION

`tree` recursively lists directory contents as an indented tree. Directories are
shown before files and entries are sorted by name.

## OPTIONS

- `-a` - include hidden entries.
- `-d` - show directories only.
- `-L LEVEL` - limit recursion depth.
- `--json` - emit JSON Lines events instead of tree text.
- `-h`, `--help` - show usage.

## JSON Output

With `--json`, `tree` emits `entry` events followed by one `summary` event:

```json
{"schema":"newos.tool.v1","tool":"tree","stream":"stdout","event":"entry","seq":1,"data":{"path":"src","name":"src","depth":0,"type":"directory"}}
{"schema":"newos.tool.v1","tool":"tree","stream":"stdout","event":"summary","seq":2,"data":{"directories":4,"files":12}}
```

Diagnostics and usage messages follow the shared `json-output` envelope.

## SEE ALSO

ls, find, du
