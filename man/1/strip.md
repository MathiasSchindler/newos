# strip(1)

## Name
**strip** - remove non-essential ELF metadata from binaries

## Synopsis
**strip** [**-o** OUTPUT] FILE ...

## Description
`strip` writes a smaller ELF output by removing section-header metadata that is
not required for execution.

With **-o**, the stripped result is written to a new file. Otherwise the input file is replaced in place.

## Examples
```sh
strip hello
strip -o hello.small hello
```

## Limitations
ELF64 little-endian executables and shared objects receive real stripping.
Relocatable object files are still rejected for safety. Non-ELF hosted formats
such as Mach-O currently fall back to a safe copy/no-op behavior instead of
failing.
