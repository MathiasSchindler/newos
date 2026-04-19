# readelf(1)

## Name
**readelf** - inspect ELF file headers, sections, and symbols

## Synopsis
**readelf** [**-h**] [**-S**] [**-s**] FILE ...

## Description
`readelf` prints structural information from ELF binaries and object files.

Supported output modes:

- **-h** — ELF file header
- **-S** — section table
- **-s** — symbol table

If no mode is selected, the ELF header is shown.

## Examples
```sh
readelf -h a.out
readelf -S -s hello.o
```

## Limitations
The current implementation supports ELF64 little-endian files, which covers the project’s Linux outputs on x86_64 and AArch64.
