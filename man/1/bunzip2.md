# BUNZIP2

## NAME

bunzip2 - decompress a bzip2-compressed file

## SYNOPSIS

```
bunzip2 file.bz2
```

## DESCRIPTION

`bunzip2` expands a `.bz2` file using the repository's current bzip2 decoder.

## CURRENT CAPABILITIES

- decompress a single `.bz2` file
- validate stream structure and CRC
- write the restored output file beside the archive

## OPTIONS

The current interface is a single-file form without additional flags.

## LIMITATIONS

- the implementation is intentionally narrow compared with full host bzip2 suites
- unsupported or malformed streams fail explicitly
- concatenated bzip2 streams, test-only mode, keep/delete policy flags, and
  stdout streaming options are not implemented yet
- multi-file workflows and recursive directory handling are outside the current
  interface

## EXAMPLES

```
bunzip2 archive.txt.bz2
```

## SEE ALSO

bzip2, gunzip, unxz
