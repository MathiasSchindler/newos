# GZIP

## NAME

gzip - compress files in gzip format

## SYNOPSIS

```text
gzip [-c] [-d] [-f] [-k] [file ...]
```

## DESCRIPTION

`gzip` compresses one or more files using the project's built-in gzip/deflate
support. It can also act in decompression mode.

## CURRENT CAPABILITIES

- gzip compression of regular files
- decompression mode with `-d`
- write to standard output with `-c`
- keep the input file with `-k`
- overwrite output when forced with `-f`

## OPTIONS

- `-c` write compressed or decompressed output to standard output
- `-d` decompress instead of compress
- `-f` force output replacement
- `-k` keep the original input file

## LIMITATIONS

- the tool focuses on core gzip workflow rather than every GNU extension
- advanced metadata and obscure stream variations may not be implemented

## EXAMPLES

```text
gzip file.txt
gzip -c file.txt > file.txt.gz
gzip -d archive.gz
```

## SEE ALSO

gunzip, bzip2, xz, tar
