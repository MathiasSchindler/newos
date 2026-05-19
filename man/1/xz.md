# XZ

## NAME

xz - compress a single file with LZMA2

## SYNOPSIS

```
xz file
```

## DESCRIPTION

The xz tool compresses a single file to `.xz` format using LZMA2. The input file remains on disk after compression.

## CURRENT CAPABILITIES

- compress one file at a time
- write output as `file.xz`
- leave the original input file untouched

## OPTIONS

None.

## LIMITATIONS

- Single-file operation only.
- No decompression flags, streaming mode, or compression-level selection are implemented.
- No integrity-check selection, memory-limit controls, threaded compression, or
  `.txz`/tar integration is provided.

## EXAMPLES

```
xz image.raw
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

unxz, gzip, bzip2
