# SPLIT

## NAME

split - split a file into pieces

## SYNOPSIS

```
split [-l COUNT | -b SIZE | -C SIZE | -n CHUNKS]
	[-a SUFFIX_LEN] [-d] [file [prefix]]
```

## DESCRIPTION

split divides a file (or standard input) into smaller pieces. Output files are named with a configurable prefix (default `x`) followed by an alphabetic or numeric suffix.

## CURRENT CAPABILITIES

- splitting by line count (`-l`)
- splitting by byte size (`-b`) with suffix multipliers (`k`, `m`, `g`)
- splitting by maximum line-bytes per chunk (`-C`)
- splitting into a fixed number of roughly equal chunks (`-n`)
- alphabetic (default) and numeric (`-d`) output suffixes
- configurable suffix length (`-a`)

## OPTIONS

- `-l COUNT` / `--lines COUNT` — put at most COUNT lines per output file
- `-b SIZE` / `--bytes SIZE` — put at most SIZE bytes per output file; SIZE may use suffixes `k` (KiB), `m` (MiB), `g` (GiB)
- `-C SIZE` / `--line-bytes SIZE` — put at most SIZE bytes per output file, but only split on line boundaries
- `-n CHUNKS` / `--number CHUNKS` — split into CHUNKS roughly equal-size files
- `-a SUFFIX_LEN` / `--suffix-length SUFFIX_LEN` — use SUFFIX_LEN characters for the suffix (default 2)
- `-d` / `--numeric-suffixes` — use numeric suffixes (00, 01, …) instead of alphabetic (aa, ab, …)

## LIMITATIONS

- no `-t` record-separator option
- no `--additional-suffix` option
- numeric and generated suffix handling covers the documented forms, not every
  GNU suffix-length and filter-command combination
- line and byte counts are parsed as simple sizes; locale-specific or
  block-device-aware behavior is not implemented

## EXAMPLES

- `split -l 100 big.txt part_` — split into 100-line chunks named `part_aa`, `part_ab`, …
- `split -b 1m archive.tar chunk_` — split into 1 MiB pieces
- `split -n 4 data.csv part` — split into 4 equal pieces
- `split -d -a 3 file.txt x` — numeric 3-digit suffix: `x000`, `x001`, …

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

cat, csplit, head, tail
