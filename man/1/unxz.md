# UNXZ

## NAME

unxz - decompress an xz file

## SYNOPSIS

```
unxz file.xz
```

## DESCRIPTION

The unxz tool decompresses a single `.xz` file to its original name. The compressed input is preserved.

## CURRENT CAPABILITIES

- decompress one `.xz` file at a time
- write output without the `.xz` suffix
- leave the compressed input file untouched

## OPTIONS

None.

## LIMITATIONS

- Single-file operation only.
- No additional options or streaming mode are implemented.
- No integrity-check selection, memory-limit controls, concatenated stream
  handling, or `.txz`/tar integration is provided.

## EXAMPLES

```
unxz image.raw.xz
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

xz, gzip, bunzip2
