# READELF

## NAME

**readelf** - inspect ELF file structure

## SYNOPSIS

```
readelf [-a] [-h] [-l] [-S] [-d] [-r] [-s] [-n] FILE ...
```

## DESCRIPTION

`readelf` prints structural information from ELF binaries and object files. On
hosted macOS builds it can also recognize Mach-O headers well enough to report
basic file information instead of failing outright.

ELF executables without a section header table are accepted. This covers tiny
freestanding outputs from the project linker, where program loading metadata is
enough to run the binary and section metadata has intentionally been omitted.

Supported output modes:

- **-a**, **--all** — all implemented ELF views
- **-h** — ELF file header
- **-l** — program headers / loadable segments
- **-S** — section table
- **-d** — dynamic section
- **-r** — relocation sections
- **-s** — symbol table
- **-n** — note sections

If no mode is selected, the ELF header is shown.

## EXAMPLES

```
readelf -h a.out
readelf -l tiny-static-tool
readelf -S -s hello.o
readelf -a /bin/ls
```

## LIMITATIONS

- Detailed section and symbol inspection currently targets ELF64 little-endian
  files, which covers the project's Linux outputs on x86_64 and AArch64.
- Mach-O support is currently limited to basic header reporting for hosted
  compatibility.
- Dynamic-section, relocation, and note views are intentionally compact. They
  cover common ELF64 Linux files but are not a complete GNU readelf clone.
- Versioning, DWARF, unwind, GOT, histogram, and archive-member views are not
  complete.
- Output formatting is diagnostic-oriented and not a full GNU readelf clone.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

