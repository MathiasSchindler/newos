# READELF

## NAME

**readelf** - inspect ELF file structure

## SYNOPSIS

```
readelf [-a] [-h] [-l] [-S] [-d] [-r] [-s] [-n] [--json] FILE ...
readelf [--json] --compare LEFT RIGHT
```

## DESCRIPTION

`readelf` prints structural information from ELF binaries and object files. On
hosted macOS builds it can also recognize Mach-O 64-bit little-endian files and
Mach-O universal binaries, reporting useful compatibility views for project
diagnostics.

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

For Mach-O inputs, `-h` prints the Mach-O header. For universal binaries it also
prints the fat-architecture table and then inspects the preferred arm64/arm64e
slice. `-l` prints load commands such as `LC_SEGMENT_64`, `LC_MAIN`,
`LC_BUILD_VERSION`, `LC_UUID`, `LC_SOURCE_VERSION`, `LC_LOAD_DYLIB`,
`LC_DYLD_CHAINED_FIXUPS`, `LC_DYLD_EXPORTS_TRIE`, `LC_FUNCTION_STARTS`,
`LC_DATA_IN_CODE`, and `LC_CODE_SIGNATURE`. `-S` prints sections from segment
commands, including reserved fields, and `-s` prints `LC_SYMTAB` when present.
`-r` decodes Mach-O arm64 relocation records from section relocation tables.
`-n` inspects embedded Mach-O code signatures when `LC_CODE_SIGNATURE` is
present. It decodes the SuperBlob slot table, CodeDirectory, identifier, CDHash,
full CodeDirectory SHA-256 digest, CMS-signature size, requirements size, and
verifies SHA-256 page hashes against the signed file range.
ELF-only views such as `-d` report that the input has no ELF dynamic section.

`--compare` auto-detects ELF and Mach-O inputs, compares their structural
summary, and compares full-file SHA-256 digests. It exits with status 0 when the
files are equivalent according to those checks and status 1 when differences are
found.

If no mode is selected, the ELF header is shown.

## EXAMPLES

```
readelf -h a.out
readelf -l tiny-static-tool
readelf -S -s hello.o
readelf -a /bin/ls
readelf -h -l -S -r build/newlinker-macos-aarch64/echo
readelf -n build/newlinker-macos-aarch64/true
readelf -h -l -S -s -n /usr/bin/true
readelf --compare build/newlinker-macos-aarch64/true build/newlinker-macos-aarch64/false
readelf --json -h -l -S -n build/newlinker-macos-aarch64/true
```

## LIMITATIONS

- Detailed ELF section and symbol inspection targets ELF64 little-endian files,
  which covers the project's Linux outputs on x86_64 and AArch64.
- Mach-O support covers universal/fat containers, 64-bit little-endian headers,
  common modern dyld load commands, `LC_SEGMENT_64` sections, arm64 relocations,
  `LC_SYMTAB` symbols, and embedded CodeDirectory signatures. It is intended for
  the project's macOS freestanding and newlinker binaries plus compact system
  binaries such as `/usr/bin/true`, not as a complete replacement for `otool` or
  `codesign`.
- Dynamic-section, relocation, and note views are intentionally compact. They
  cover common ELF64 Linux files but are not a complete GNU readelf clone.
- Versioning, DWARF, unwind, GOT, histogram, and archive-member views are not
  complete.
- Output formatting is diagnostic-oriented and not a full GNU readelf clone.

## JSON Output

With `--json`, `readelf` emits JSON Lines using the common envelope documented
in `json-output`. Implemented events include `elf_header`, `macho_fat_arch`,
`macho_header`, `macho_load_command`, `macho_section`, `macho_symbol`,
`macho_relocation`, `macho_code_signature`, `compare_difference`, and
`compare_summary`.

Example event:

```json
{"schema":"newos.tool.v1","tool":"readelf","stream":"stdout","event":"macho_code_signature","seq":1,"data":{"file":"true","present":true,"hashes_verified":true,"checked":1,"mismatches":0}}
```

`--compare --json` emits one `compare_difference` event per differing field and
a final `compare_summary` event. A non-zero exit status still indicates that the
inputs differ.

