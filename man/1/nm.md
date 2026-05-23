# NM

## NAME

nm - list ELF64 symbols

## SYNOPSIS

```
nm [-n] [-p] [-u] [-g] FILE ...
```

## DESCRIPTION

`nm` prints symbols from ELF64 little-endian files. It is intended for the
project's compiler, linker, and profiler workflows, especially producing
`nm -n` symbol maps for `profiler`.

## OPTIONS

- `-n`, `--numeric-sort` - sort by address. This is the default.
- `-p`, `--no-sort` - keep symbol-table order.
- `-u`, `--undefined-only` - show only undefined symbols.
- `-g`, `--extern-only` - show only non-local symbols.
- `-h`, `--help` - show usage.

## LIMITATIONS

Only ELF64 little-endian symbol tables are supported. Archives, DWARF, Mach-O,
and PE/COFF symbol tables are outside the initial scope.

## SEE ALSO

size, readelf, objdump, profiler, linker
