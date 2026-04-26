# FREE

## NAME

free - display memory usage

## SYNOPSIS

```
free [-b|-k|-m|-g|-h] [-w] [-t]
```

## DESCRIPTION

free reports the total, used, free, shared, buffer/cache, and available memory, as well as swap usage. Values are read from the platform memory information interface.

## CURRENT CAPABILITIES

- reporting physical memory and swap in a tabular format
- multiple unit modes: bytes, kibibytes, mebibytes, gibibytes, or human-readable
- wide output separating buffers and cache into separate columns
- totals row combining physical and swap

## OPTIONS

- `-b` — show values in bytes
- `-k` — show values in kibibytes (KiB)
- `-m` — show values in mebibytes (MiB)
- `-g` — show values in gibibytes (GiB)
- `-h` — show values in a human-readable unit chosen automatically
- `-w` — wide mode: show buffers and cache in separate columns
- `-t` — add a totals row at the bottom

## LIMITATIONS

- values are read once at invocation; no continuous (`-s INTERVAL`) update mode
- shared memory reporting depends on platform availability; may show 0 if not supported
- no `--si` (powers of 1000) mode

## EXAMPLES

- `free` — show memory in default units (KiB)
- `free -h` — human-readable output
- `free -m -t` — mebibytes with a totals row
- `free -w` — separate buffers and cache columns

## SEE ALSO

uptime, ps, pstree
