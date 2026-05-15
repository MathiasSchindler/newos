# EXPACK

## NAME

expack - pack an ELF executable while keeping it directly executable

## SYNOPSIS

`expack` [`-q`] `INPUT` `OUTPUT`

## DESCRIPTION

`expack` creates a new executable ELF file from an existing x86-64 ELF executable or PIE.
The output contains a small Linux x86-64 unpacking stub and an encoded copy of the input.
When run, the stub reconstructs the original executable in an anonymous in-memory file and executes it with the original argument vector and environment.

The first packer format is intentionally small and local to this project. It uses a fixed-window LZSS stream with a 4 KiB history window and 3..18 byte matches. The generated stub contains a compact decoder, reconstructs the original executable in anonymous memory, writes it to a `memfd`, and then executes that file.

LZSS is used here because its decompressor is small enough for an executable stub while still finding repeated code, strings, alignment bytes, and zero-heavy regions in static binaries. It is a foundation for future stronger or selectable codecs rather than a UPX-compatible format.

## OPTIONS

| Option | Description |
| --- | --- |
| `-q`, `--quiet` | Do not print the size summary. |
| `-h`, `--help` | Show usage. |

## LIMITATIONS

- output stubs currently target Linux x86-64 only
- inputs must be ELF64 little-endian x86-64 `EXEC` or `DYN` files
- the packed executable requires Linux `memfd_create` and `execveat`
- this is not a UPX-compatible file format
- the LZSS codec is simple and favors stub size over maximum compression ratio

## SEE ALSO

strip, readelf, gzip, xz, project-layout
