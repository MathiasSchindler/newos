# BUNZIP2

## NAME

bunzip2 - decompress a bzip2-compressed file

## SYNOPSIS

```
bunzip2 file.bz2
```

## DESCRIPTION

`bunzip2` expands a `.bz2` file using the repository's shared bzip2 decoder.

## CURRENT CAPABILITIES

- decompress a single `.bz2` file
- decode standard non-randomized `BZh1` through `BZh9` bzip2 streams, including
  Wikimedia dump files
- decode the repository's older compact `BZh0` test format for compatibility
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

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

bzip2, gunzip, unxz
