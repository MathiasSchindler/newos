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

## EXAMPLES

```
xz image.raw
```

## SEE ALSO

unxz, gzip, bzip2
