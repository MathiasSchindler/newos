# SIZE

## NAME

size - summarize ELF64 and Mach-O text, data, and bss sizes

## SYNOPSIS

```
size [--json] FILE ...
```

## DESCRIPTION

`size` reads ELF64 little-endian files, Mach-O 64-bit little-endian files, and
Mach-O universal binaries with an arm64/arm64e slice. It prints text, data, bss,
total decimal, total hexadecimal, file size, and file name. It is useful for
checking the effect of compiler flags, LTO, and newlinker size optimizations.

For Mach-O inputs, `size` reads `LC_SEGMENT_64` section metadata. `__TEXT`
sections count as text, writable initialized sections count as data, and
zero-fill sections such as `__DATA,__bss` count as bss. `__LINKEDIT`, load
commands, and code signatures are reflected in the file-size column but are not
included in text/data/bss payload totals.

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

- ELF64 support depends on section headers. Compact stripped ELF executables
	without section headers cannot be broken down into text/data/bss.
- Mach-O support covers 64-bit little-endian files with `LC_SEGMENT_64`
	sections, including the arm64 binaries produced by the macOS newlinker path
	and selected arm64/arm64e slices from universal binaries.
- Mach-O totals are section-payload totals, not rounded segment or code-signature
	sizes.

## SEE ALSO

nm, readelf, objdump, linker, profiler
