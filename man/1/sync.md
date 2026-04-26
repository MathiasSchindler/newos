# SYNC

## NAME

sync - flush filesystem caches to disk

## SYNOPSIS

```
sync [-d] [-v] [file ...]
```

## DESCRIPTION

sync requests that the kernel flush all pending writes to persistent storage.
When file arguments are given, only those paths are synced; otherwise all
filesystems are synced.

## CURRENT CAPABILITIES

- syncing all filesystems (no arguments)
- syncing specific files
- data-only sync mode for named paths (metadata not guaranteed)
- verbose mode reporting each file synced, or the global sync request

## OPTIONS

- `-d` / `--data` — sync named file data only; when no paths are provided the command falls back to a normal global sync request
- `-v` / `--verbose` — print each filename as it is synced, or a single confirmation when syncing all filesystems

## LIMITATIONS

- success of the underlying sync is determined by the operating system; sync merely requests the flush
- not all platforms distinguish between data-only and full sync; `-d` may behave identically to a full sync on some systems

## EXAMPLES

- `sync` — flush all filesystems
- `sync -v` — flush all filesystems and print a confirmation line
- `sync -v /dev/sda1` — sync a specific device verbosely
- `sync -d bigfile.dat` — data-only sync of a file

## SEE ALSO

truncate, dd
