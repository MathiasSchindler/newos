# NM

## NAME

nm - list object-file symbols

## SYNOPSIS

```
nm [-n] [-p] [-u] [-g] [--json] FILE ...
```

## DESCRIPTION

`nm` prints symbols from ELF64 little-endian files and Mach-O 64-bit
little-endian files. It is intended for the project's compiler, linker, and
profiler workflows, especially producing `nm -n` symbol maps for `profiler`.
For Mach-O, `nm` reads `LC_SYMTAB`, classifies common section-backed text, data,
bss, absolute, indirect, undefined, private, external, and weak symbols, and
works on both clang-produced objects and project-linked executables when a
symbol table is present. For Mach-O universal binaries, `nm` inspects the
preferred arm64/arm64e slice.

## OPTIONS

- `-n`, `--numeric-sort` - sort by address. This is the default.
- `-p`, `--no-sort` - keep symbol-table order.
- `-u`, `--undefined-only` - show only undefined symbols.
- `-g`, `--extern-only` - show only non-local symbols.
- `--json` - emit JSON Lines events instead of text.
- `-h`, `--help` - show usage.

## JSON Output

With `--json`, `nm` emits one `symbol` event per symbol:

```json
{"schema":"newos.tool.v1","tool":"nm","stream":"stdout","event":"symbol","seq":1,"data":{"file":"a.out","format":"elf","name":"main","type":"T","bind":"global","defined":true,"value":4198400,"size":42}}
```

`value` is `null` for undefined symbols. Diagnostics and usage messages follow
the shared `json-output` envelope.

## LIMITATIONS

ELF64 little-endian symbol tables and Mach-O `LC_SYMTAB` symbols are supported.
Mach-O universal binaries are supported by selecting one arm64/arm64e slice.
Archives, DWARF, and PE/COFF symbol tables are outside the initial scope.

## SEE ALSO

size, readelf, objdump, profiler, linker
