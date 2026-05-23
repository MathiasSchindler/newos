# SIZE

## NAME

size - summarize ELF64 text, data, and bss sizes

## SYNOPSIS

```
size FILE ...
```

## DESCRIPTION

`size` reads ELF64 little-endian files and prints text, data, bss, total decimal,
total hexadecimal, file size, and file name. It is useful for checking the effect
of compiler flags, LTO, and newlinker size optimizations.

## OUTPUT

```
text    data    bss     dec     hex     file    name
```

## LIMITATIONS

Only ELF64 little-endian section tables are supported. Compact stripped
executables without section headers cannot be broken down into text/data/bss.

## SEE ALSO

nm, readelf, objdump, linker, profiler
