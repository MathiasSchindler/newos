# BZIP2

## NAME

bzip2 - compress a file with the repository's bzip2 support

## SYNOPSIS

```
bzip2 file
```

## DESCRIPTION

`bzip2` compresses a single input file into bzip2 format using the current
project implementation.

## CURRENT CAPABILITIES

- compress a single file argument
- create a `.bz2` output file
- detect common input and output errors

## OPTIONS

The current interface is intentionally minimal and takes a single file operand.

## LIMITATIONS

- no broad GNU-style flag surface is implemented yet
- multi-file and streaming workflows are narrower than host tools
- compression-level selection, keep/delete policy flags, test mode, and stdout
  output are not implemented yet
- this command compresses regular files; use `tar` first when preserving
  directory structure, ownership, or multiple paths matters

## EXAMPLES

```
bzip2 notes.txt
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

bunzip2, gzip, xz
