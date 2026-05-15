# STRIP

## NAME

**strip** - remove non-essential object-file metadata from binaries

## SYNOPSIS

```
strip [-nv] [--strip-all|--strip-debug] [-o OUTPUT] FILE ...
```

## DESCRIPTION

`strip` writes a smaller or cleaner object file by removing metadata that is not
required for execution when the format can be handled safely. ELF64
little-endian executables and shared objects receive real section-header
 metadata stripping. PE/COFF files have the COFF symbol-table pointer, symbol
count, and debug data-directory entry cleared when present. Mach-O inputs are
recognized and analyzed, with load-command, symbol-table, and code-signature
diagnostics in verbose mode, but Mach-O load commands and code signatures are not
rewritten yet. `ar` archives are recognized and analyzed in verbose/dry-run mode,
but archive members are not rewritten yet.

With **-o**, the stripped result is written to a new file. Otherwise the input file is replaced in place.

By default `strip` behaves like `--strip-all`. `--strip-debug` is accepted as an
explicit mode, but currently maps to the same conservative executable stripping
path for supported ELF files because debug-only section-table rewriting is not
implemented yet.

## OPTIONS

- `-o OUTPUT` — write the result to OUTPUT instead of replacing the input
- `-n`, `--dry-run` — report what would be done without writing an output file
- `-v`, `--verbose` — report the detected format, mode, input and output sizes,
  action taken, and format-specific diagnostics when available
- `--strip-all` — remove all safely removable non-load metadata (default)
- `--strip-debug` — request debug-only stripping where supported

## EXAMPLES

```
strip hello
strip -o hello.small hello
strip -v --dry-run app.exe
strip --strip-debug -o app.nodebug app
```

## LIMITATIONS

- ELF64 little-endian executables and shared objects receive real stripping by
  removing trailing section-header metadata and clearing section-header fields in
  the ELF header.
- Relocatable object files are rejected for safety until section-table,
  relocation, and symbol references can be rewritten correctly.
- Mach-O 64-bit little-endian files are recognized and verbose output reports
  load-command count, `LC_SYMTAB`, and code-signature information. Rewriting
  `LC_SYMTAB`, `LC_DYSYMTAB`, debug data, `__LINKEDIT`, and code signatures is
  not implemented yet, so Mach-O stripping remains a safe copy/no-op.
- PE/COFF files support clearing the COFF symbol-table pointer/count and the PE
  debug data-directory entry. Certificate tables, resources, imports, exports,
  relocations, debug payload reclamation, and checksums are not rewritten.
- `ar` archives are recognized and counted for verbose/dry-run diagnostics, but
  member rewriting is not implemented yet.
- Fine-grained symbol selection and true debug-only section rewriting are not
  implemented yet.
