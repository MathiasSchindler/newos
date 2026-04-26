# GZIP

## NAME

gzip - compress files in gzip format

## SYNOPSIS

```
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
- process multiple input files in one invocation

## OPTIONS

- `-c` write compressed or decompressed output to standard output
- `-d` decompress instead of compress
- `-f` force output replacement
- `-k` keep the original input file

## LIMITATIONS

- only gzip streams are handled; this is not a general `.zip` extractor
- no recursive directory compression mode, test mode (`-t`), or listing mode (`-l`) is implemented
- directory trees usually need to be packed first with `tar`

## EXAMPLES

```
gzip file.txt
gzip -c file.txt > file.txt.gz
gzip -d archive.gz
tar -cf backup.tar src && gzip backup.tar
```

## SEE ALSO

gunzip, bzip2, xz, tar
