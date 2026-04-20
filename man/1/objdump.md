# objdump(1)

## Name
**objdump** - summarize and dump ELF object contents

## Synopsis
**objdump** [**-f**] [**-h**] [**-s**] [**-t**] FILE ...

## Description
`objdump` provides a compact view of ELF files. On hosted macOS builds it also
reports basic Mach-O file summaries so the inspection workflow remains usable.

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
Full section, symbol, and content dumping is centered on ELF64 little-endian
inputs. Mach-O handling is currently limited to file-summary reporting rather
than deep disassembly or section decoding.
