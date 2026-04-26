# READELF

## NAME

**readelf** - inspect ELF file headers, sections, and symbols

## SYNOPSIS

```
readelf [-h] [-S] [-s] FILE ...
```

## DESCRIPTION

`readelf` prints structural information from ELF binaries and object files. On
hosted macOS builds it can also recognize Mach-O headers well enough to report
basic file information instead of failing outright.

Supported output modes:

- **-h** — ELF file header
- **-S** — section table
- **-s** — symbol table

If no mode is selected, the ELF header is shown.

## EXAMPLES

```
readelf -h a.out
readelf -S -s hello.o
```

## LIMITATIONS

Detailed section and symbol inspection currently targets ELF64 little-endian files, which covers the project's Linux outputs on x86_64 and AArch64. Mach-O support is currently limited to basic header reporting for hosted compatibility.
