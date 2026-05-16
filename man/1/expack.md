# EXPACK

## NAME

expack - pack an ELF executable while keeping it directly executable

## SYNOPSIS

`expack` [`-q`] `INPUT` `OUTPUT`

`expack` `--analyze` `INPUT`

`expack` `--macho-container` `INPUT` `OUTPUT`

## DESCRIPTION

`expack` creates a new executable file from an existing executable image. The current packable backend is implemented for x86-64 ELF executable and PIE inputs, and the current output container is a Linux x86-64 ELF stub.
Before compression the ELF backend reconstructs a canonical execution image from the input ELF header, program header table, and loadable file ranges. Load segment file offsets are compacted while preserving each segment's virtual-address alignment rule. Program headers that point inside moved load segments are adjusted. Section headers, unreferenced trailing data, and gaps outside the executable image are omitted or zeroed.
The output contains a small unpacking stub and an encoded copy of that execution image.
When run, the stub reconstructs the preprocessed executable image in an anonymous in-memory file and executes it with the original argument vector and environment.

The first packer formats are intentionally small and local to this project. The generated stub contains the selected decoder, reconstructs the original executable in anonymous memory, writes it to a `memfd`, and then executes that file.

LZSS is used here because its decompressor is small enough for an executable stub while still finding repeated code, strings, alignment bytes, and zero-heavy regions in static binaries. `expack` tries multiple small decoder profiles, currently an 8 KiB history window with 3..10 byte matches, a 4 KiB history window with 3..18 byte matches, a 2 KiB history window with 3..34 byte matches, and a 1 KiB history window with 3..66 byte matches. It also tries a repeat-distance LZ variant where explicit matches establish an offset and later matches may reuse that offset with a one-byte token.

For x86-64 code-heavy images, `expack` also tries a reversible branch transform before LZSS. Relative `call`, `jmp`, and near conditional-branch displacements are converted to file-position targets before compression, and the selected stub converts them back after decompression and before execution. The transform is applied to the whole reconstructed image so the inverse pass restores the same byte positions, including any accidental opcode-looking bytes in data.

Use `--analyze` to inspect the exact portfolio decision without writing an output file. The report shows the input size, reconstructed image size, every candidate's payload, stub, and final packed size, and the selected winner. This is useful when tuning codecs because decoder size is counted in the reported result.

The executable-format layer is intentionally separate from the codec selection and packed-output writer so Mach-O and PE/COFF image backends can be added without rewriting the compression portfolio. Mach-O 64-bit executable inputs can be analyzed and compressed as exact executable images; this keeps code-signature bytes in the payload and avoids rewriting load commands. PE/COFF inputs are recognized and rejected with an explicit unsupported-backend diagnostic.

`--macho-container` is a development step toward native Mach-O output. It writes a Mach-O executable-shaped container for an x86-64 or arm64 Mach-O input with a `__TEXT,__expack` section containing an `EXPACKM1` metadata block followed by the payload. For arm64 Mach-O inputs on Darwin, the container can execute LZREP-compressed payloads; its native runner decompresses the exact executable image to a temporary executable file and executes it with the original argument vector and environment. If the selected codec does not have an arm64 Darwin runner yet, the container falls back to a raw exact-image payload. For x86-64 Mach-O inputs, the container is still a signed layout and metadata prototype with a placeholder entry point. The container includes linker-like `__PAGEZERO`, `__TEXT`, `__LINKEDIT`, dyld, build-version, and code-signature load commands. `expack` emits a minimal ad-hoc CodeDirectory signature using SHA-256 page hashes; Apple's `codesign --verify --strict` can be used as an oracle for the generated signature. This file is not the final general Mach-O output path yet because only the arm64 LZREP runner exists.

Tiny literal/zero-run and literal/byte-run codecs are also tried for executable images dominated by long repeated regions. The compressor can spend extra CPU on optimal match selection because packed binaries are usually produced once and decompressed many times. `expack` ships the decoder that produced the smallest complete packed file, including stub overhead. It is a foundation for future stronger or selectable codecs rather than a UPX-compatible format.

## OPTIONS

| Option | Description |
| --- | --- |
| `-q`, `--quiet` | Do not print the size summary. |
| `--analyze` | Report candidate sizes for `INPUT` without creating a packed executable. |
| `--macho-container` | Write a prototype Mach-O container; arm64 Mach-O inputs can run LZREP-compressed payloads. |
| `-h`, `--help` | Show usage. |

## LIMITATIONS

- output stubs currently target Linux x86-64 ELF only
- packable inputs must be ELF64 little-endian x86-64 `EXEC` or `DYN` files
- Mach-O 64-bit executable inputs support `--analyze` and `--macho-container`; arm64 containers can run LZREP-compressed payloads and use raw payloads as a fallback
- PE/COFF inputs are recognized but not packable yet
- the packed executable requires Linux `memfd_create` and `execveat`
- section headers and other bytes not referenced by program headers are not preserved
- load segment file offsets may be canonicalized in the reconstructed image
- this is not a UPX-compatible file format
- the codecs are simple and favor stub size over maximum compression ratio

## SEE ALSO

strip, readelf, gzip, xz, project-layout
