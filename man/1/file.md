# FILE

## NAME

file - determine file type

## SYNOPSIS

```
file [-i] [-L|-h] [file ...]
```

## DESCRIPTION

`file` tests each argument and prints a description of its type based on
magic byte patterns and content heuristics.

## CURRENT CAPABILITIES

Recognises the following types by magic bytes or content inspection:

- ELF executable and object files
- 7-Zip, bzip2, xz archives
- Mach-O binaries
- PE/COFF (Windows) executables
- tar archives, ar static libraries
- Python, Perl, awk, and shell scripts (shebang detection)
- AVI video, HTML, plain text
- Empty files

## OPTIONS

- `-i`, `--mime`, `--mime-type` — print MIME type instead of description
- `-L`, `--dereference` — follow symlinks
- `-h`, `--no-dereference` — do not follow symlinks (default)

## LIMITATIONS

- No magic database (`/etc/magic`, `magic.mgc`); recognition is compiled-in.
- Many file types are not recognised and fall back to
  `application/octet-stream`.
- No `-z` (decompress before testing).

## EXAMPLES

```
file /bin/ls
file -i image.png
file -L /proc/self/exe
file *.c
```

## SEE ALSO

hexdump, strings, stat
