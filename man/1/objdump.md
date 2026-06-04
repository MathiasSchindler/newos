# OBJDUMP

## NAME

**objdump** - summarize and dump object file contents

## SYNOPSIS

```
objdump [-f] [-h] [-s] [-t] [-r] [--json] FILE ...
```

## DESCRIPTION

`objdump` provides a compact view of object and executable container formats.
ELF64 little-endian inputs have the deepest support; Mach-O 64-bit and PE/COFF
inputs support file summaries, section tables, and raw section content dumps.
Mach-O symbol tables are printed when an `LC_SYMTAB` load command is present,
and Mach-O arm64 relocation tables are decoded with `-r`.

Supported output modes:

- **-f** — file format and entry point summary
- **-h** — section table
- **-s** — raw section contents in hexadecimal
- **-t** — symbol table where supported
- **-r** — relocation records where supported
- **--json** — emit structured JSON Lines events for metadata views

If no mode is selected, the file summary and section table are shown.

## EXAMPLES

```
objdump -f -h kernel.o
objdump -s -t app
objdump -f -h app.exe
objdump -f -h -t -r build/newlinker-macos-aarch64/echo
objdump --json -f -h -t -r build/newlinker-macos-aarch64/echo
```

## LIMITATIONS

- Symbol dumping supports ELF64 section symbol tables and Mach-O `LC_SYMTAB`.
- Mach-O handling covers 64-bit little-endian files with `LC_SEGMENT_64`
  sections. Raw dumps skip zero-fill sections because they have no file bytes.
- PE/COFF handling covers PE32 and PE32+ executable images with standard section
  tables.
- Instruction disassembly, ELF relocation decoding, DWARF display, archive
  member traversal, and target-selection options are not implemented yet.
- In JSON mode, raw section byte dumps requested by `-s` are omitted so stdout
  remains valid JSON Lines.
- Output is intended for project diagnostics and may not match GNU binutils
  formatting or option compatibility.

## JSON Output

With `--json`, `objdump` emits JSON Lines using the common envelope documented
in `json-output`. Implemented events include `file_header`, `section`, `symbol`,
and `relocation`. ELF inputs currently emit file and section metadata. Mach-O
inputs emit file, section, `LC_SYMTAB` symbols, and arm64 relocation events.

Example event:

```json
{"schema":"newos.tool.v1","tool":"objdump","stream":"stdout","event":"section","seq":1,"data":{"file":"true","format":"mach-o-64","index":0,"name":"__TEXT,__text","addr":4294968040,"offset":744,"size":10024,"type":"regular","flags":2147484672}}
```

