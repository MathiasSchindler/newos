# objdump(1)

## Name
**objdump** - summarize and dump ELF object contents

## Synopsis
**objdump** [**-f**] [**-h**] [**-s**] [**-t**] FILE ...

## Description
`objdump` provides a compact view of ELF files.

Supported output modes:

- **-f** — file format and entry point summary
- **-h** — section table
- **-s** — raw section contents in hexadecimal
- **-t** — symbol table

If no mode is selected, the file summary and section table are shown.

## Examples
```sh
objdump -f -h kernel.o
objdump -s -t app
```

## Limitations
This implementation currently targets ELF64 little-endian inputs and focuses on section/symbol inspection rather than full instruction disassembly.
