# OBJDUMP

## NAME

**objdump** - summarize and dump ELF object contents

## SYNOPSIS

```
objdump [-f] [-h] [-s] [-t] FILE ...
```

## DESCRIPTION

`objdump` provides a compact view of ELF files. On hosted macOS builds it also
reports basic Mach-O file summaries so the inspection workflow remains usable.

Supported output modes:

- **-f** — file format and entry point summary
- **-h** — section table
- **-s** — raw section contents in hexadecimal
- **-t** — symbol table

If no mode is selected, the file summary and section table are shown.

## EXAMPLES

```
objdump -f -h kernel.o
objdump -s -t app
```

## LIMITATIONS

Full section, symbol, and content dumping is centered on ELF64 little-endian inputs. Mach-O handling is currently limited to file-summary reporting rather than deep disassembly or section decoding.
