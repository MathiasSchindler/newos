# STRIP

## NAME

**strip** - remove non-essential ELF metadata from binaries

## SYNOPSIS

```
strip [-o OUTPUT] FILE ...
```

## DESCRIPTION

`strip` writes a smaller ELF output by removing section-header metadata that is
not required for execution.

With **-o**, the stripped result is written to a new file. Otherwise the input file is replaced in place.

## EXAMPLES

```
strip hello
strip -o hello.small hello
```

## LIMITATIONS

ELF64 little-endian executables and shared objects receive real stripping. Relocatable object files are still rejected for safety. Non-ELF hosted formats such as Mach-O currently fall back to a safe copy/no-op behavior instead of failing.
