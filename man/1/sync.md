# SYNC

## NAME

sync - flush filesystem caches to disk

## SYNOPSIS

sync [-d] [-v] [file ...]

## DESCRIPTION

sync requests that the kernel flush all pending writes to persistent storage. When file arguments are given, only those files are synced; otherwise all filesystems are synced.

## CURRENT CAPABILITIES

- syncing all filesystems (no arguments)
- syncing specific files
- data-only sync mode (metadata not guaranteed)
- verbose mode reporting each file synced

## OPTIONS

- `-d` / `--data` — sync file data only; metadata (timestamps, size) may not be flushed
- `-v` / `--verbose` — print each filename as it is synced

## LIMITATIONS

- success of the underlying sync is determined by the operating system; sync merely requests the flush
- not all platforms distinguish between data-only and full sync; `-d` may behave identically to a full sync on some systems

## EXAMPLES

- `sync` — flush all filesystems
- `sync -v /dev/sda1` — sync a specific device verbosely
- `sync -d bigfile.dat` — data-only sync of a file

## SEE ALSO

truncate, dd
