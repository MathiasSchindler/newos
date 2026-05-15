# OBJDUMP

## NAME

**objdump** - summarize and dump object file contents

## SYNOPSIS

```
objdump [-f] [-h] [-s] [-t] FILE ...
```

## DESCRIPTION

`objdump` provides a compact view of object and executable container formats.
ELF64 little-endian inputs have the deepest support; Mach-O 64-bit and PE/COFF
inputs support file summaries, section tables, and raw section content dumps.

Supported output modes:

- **-f** — file format and entry point summary
- **-h** — section table
- **-s** — raw section contents in hexadecimal
- **-t** — symbol table where supported

If no mode is selected, the file summary and section table are shown.

## EXAMPLES

```
objdump -f -h kernel.o
objdump -s -t app
objdump -f -h app.exe
```

## LIMITATIONS

- Symbol dumping is currently centered on ELF64 little-endian inputs.
- Mach-O handling covers 64-bit little-endian files with `LC_SEGMENT_64`
  sections.
- PE/COFF handling covers PE32 and PE32+ executable images with standard section
  tables.
- Instruction disassembly, relocation decoding, DWARF display, archive member
  traversal, and target-selection options are not implemented yet.
- Output is intended for project diagnostics and may not match GNU binutils
  formatting or option compatibility.
