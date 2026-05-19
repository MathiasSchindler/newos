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

- Detailed section and symbol inspection currently targets ELF64 little-endian
  files, which covers the project's Linux outputs on x86_64 and AArch64.
- Mach-O support is currently limited to basic header reporting for hosted
  compatibility.
- Relocation, dynamic-section, note, versioning, DWARF, and archive-member
  views are not complete.
- Output formatting is diagnostic-oriented and not a full GNU readelf clone.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

