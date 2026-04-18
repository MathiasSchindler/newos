# BZIP2

## NAME

bzip2 - compress a file with the repository's bzip2 support

## SYNOPSIS

```text
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

## EXAMPLES

```text
bzip2 notes.txt
```

## SEE ALSO

bunzip2, gzip, xz
