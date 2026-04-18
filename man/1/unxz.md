# UNXZ

## NAME

unxz - decompress an xz file

## SYNOPSIS

unxz file.xz

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

## EXAMPLES

- `unxz image.raw.xz`

## SEE ALSO

xz, gzip, bunzip2
