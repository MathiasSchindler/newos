# EXPACK

## NAME

expack - pack an ELF executable while keeping it directly executable

## SYNOPSIS

`expack` [`-q`] `INPUT` `OUTPUT`

## DESCRIPTION

`expack` creates a new executable ELF file from an existing x86-64 ELF executable or PIE.
The output contains a small Linux x86-64 unpacking stub and an encoded copy of the input.
When run, the stub reconstructs the original executable in an anonymous in-memory file and executes it with the original argument vector and environment.

The first packer formats are intentionally small and local to this project. The generated stub contains the selected decoder, reconstructs the original executable in anonymous memory, writes it to a `memfd`, and then executes that file.

LZSS is used here because its decompressor is small enough for an executable stub while still finding repeated code, strings, alignment bytes, and zero-heavy regions in static binaries. `expack` tries multiple small decoder profiles, currently an 8 KiB history window with 3..10 byte matches, a 4 KiB history window with 3..18 byte matches, a 2 KiB history window with 3..34 byte matches, and a 1 KiB history window with 3..66 byte matches. It also tries a tiny literal/zero-run codec for files where long zero regions dominate. The compressor can spend extra CPU on optimal match selection because packed binaries are usually produced once and decompressed many times. `expack` ships the decoder that produced the smallest complete packed file, including stub overhead. It is a foundation for future stronger or selectable codecs rather than a UPX-compatible format.

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
- the codecs are simple and favor stub size over maximum compression ratio

## SEE ALSO

strip, readelf, gzip, xz, project-layout
