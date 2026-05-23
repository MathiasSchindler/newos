# SIZE

## NAME

size - summarize ELF64 text, data, and bss sizes

## SYNOPSIS

```
size [--json] FILE ...
```

## DESCRIPTION

`size` reads ELF64 little-endian files and prints text, data, bss, total decimal,
total hexadecimal, file size, and file name. It is useful for checking the effect
of compiler flags, LTO, and newlinker size optimizations.

## OPTIONS

- `--json` - emit JSON Lines events instead of the text table.
- `-h`, `--help` - show usage.

## OUTPUT

```
text    data    bss     dec     hex     file    name
```

## JSON Output

With `--json`, `size` emits one `size` event per file:

```json
{"schema":"newos.tool.v1","tool":"size","stream":"stdout","event":"size","seq":1,"data":{"file":"app","text":1234,"data_size":56,"bss":78,"total":1368,"file_size":4096}}
```

Diagnostics and usage messages follow the shared `json-output` envelope.

## LIMITATIONS

Only ELF64 little-endian section tables are supported. Compact stripped
executables without section headers cannot be broken down into text/data/bss.

## SEE ALSO

nm, readelf, objdump, linker, profiler
