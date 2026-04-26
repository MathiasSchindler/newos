# GUNZIP

## NAME

gunzip - decompress gzip-compressed files

## SYNOPSIS

```
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
- decompress multiple archives in one invocation

## OPTIONS

- `-c` send decompressed data to standard output
- `-f` force overwrite behavior
- `-k` keep the input archive

## LIMITATIONS

- only gzip-formatted streams are accepted
- no archive listing or test-only modes are implemented
- unsupported or malformed deflate blocks are rejected explicitly rather than guessed around

## EXAMPLES

```
gunzip logs.gz
gunzip -c logs.gz > logs.txt
gunzip -k backup.tar.gz
```

## SEE ALSO

gzip, bzip2, bunzip2
