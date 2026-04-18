# GUNZIP

## NAME

gunzip - decompress gzip-compressed files

## SYNOPSIS

```text
gunzip [-c] [-f] [-k] [file.gz ...]
```

## DESCRIPTION

`gunzip` expands gzip-compressed files and performs integrity checks while
decoding the stream.

## CURRENT CAPABILITIES

- read `.gz` files
- verify gzip header, CRC, and stored size
- write to standard output with `-c`
- keep the compressed input with `-k`
- replace existing output when forced with `-f`

## OPTIONS

- `-c` send decompressed data to standard output
- `-f` force overwrite behavior
- `-k` keep the input archive

## LIMITATIONS

- intentionally smaller than full GNU gzip compatibility
- unsupported or malformed deflate blocks are rejected explicitly

## EXAMPLES

```text
gunzip logs.gz
gunzip -c logs.gz > logs.txt
```

## SEE ALSO

gzip, bzip2, bunzip2
