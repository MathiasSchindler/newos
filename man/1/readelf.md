# READELF

## NAME

**readelf** - inspect ELF file structure

## SYNOPSIS

```
readelf [-a] [-h] [-l] [-S] [-d] [-r] [-s] [-n] FILE ...
```

## DESCRIPTION

`readelf` prints structural information from ELF binaries and object files. On
hosted macOS builds it can also recognize Mach-O 64-bit little-endian files and
report useful compatibility views for project diagnostics.

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

For Mach-O inputs, `-h` prints the Mach-O header, `-l` prints load commands such
as `LC_SEGMENT_64`, `LC_MAIN`, `LC_BUILD_VERSION`, and `LC_CODE_SIGNATURE`, `-S`
prints sections from segment commands, and `-s` prints `LC_SYMTAB` when present.
ELF-only views such as `-d` report that the input has no ELF dynamic section.

If no mode is selected, the ELF header is shown.

## EXAMPLES

```
readelf -h a.out
readelf -l tiny-static-tool
readelf -S -s hello.o
readelf -a /bin/ls
readelf -h -l -S build/newlinker-macos-aarch64/echo
```

## LIMITATIONS

- Detailed ELF section and symbol inspection targets ELF64 little-endian files,
  which covers the project's Linux outputs on x86_64 and AArch64.
- Mach-O support covers 64-bit little-endian headers, load commands,
  `LC_SEGMENT_64` sections, and `LC_SYMTAB` symbols. It is intended for the
  project's macOS freestanding and newlinker binaries, not as a complete
  replacement for `otool`.
- Dynamic-section, relocation, and note views are intentionally compact. They
  cover common ELF64 Linux files but are not a complete GNU readelf clone.
- Versioning, DWARF, unwind, GOT, histogram, and archive-member views are not
  complete.
- Output formatting is diagnostic-oriented and not a full GNU readelf clone.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

