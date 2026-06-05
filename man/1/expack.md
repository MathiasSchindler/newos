# EXPACK

## NAME

expack - pack an executable while keeping it directly executable

## SYNOPSIS

`expack` [`-q`] [`--all`] `INPUT` [`OUTPUT`]

`expack` `--analyze` [`--all`] `INPUT`

`expack` `--macho-container` [`--all`] `INPUT` [`OUTPUT`]

## DESCRIPTION

`expack` creates a new executable file from an existing executable image. If `OUTPUT` is omitted, the output path is `INPUT-pack`. The output format is selected from the detected input format: x86-64 ELF inputs produce a Linux x86-64 ELF stub, Mach-O inputs produce a Mach-O container, and PE32+ x86-64 inputs produce a PE/COFF self-extracting container.
The current ELF backend is implemented for x86-64 ELF executable and PIE inputs.
Before compression the ELF backend reconstructs a canonical execution image from the input ELF header, program header table, and loadable file ranges. Load segment file offsets are compacted while preserving each segment's virtual-address alignment rule. Program headers that point inside moved load segments are adjusted. Section headers, unreferenced trailing data, and gaps outside the executable image are omitted or zeroed. ELF inputs that intentionally overlap the ELF header tail with the first program header are preserved by treating section-header fields as ignored when no section table is present.
The output contains a small unpacking stub and an encoded copy of that execution image.
When run, the stub reconstructs the preprocessed executable image in an anonymous in-memory file and executes it with the original argument vector and environment.

The first packer formats are intentionally small and local to this project. The generated stub contains the selected decoder, reconstructs the original executable in anonymous memory, writes it to a `memfd`, and then executes that file.

LZSS is used here because its decompressor is small enough for an executable stub while still finding repeated code, strings, alignment bytes, and zero-heavy regions in static binaries. `expack` tries multiple small decoder profiles, currently an 8 KiB history window with 3..10 byte matches, a 4 KiB history window with 3..18 byte matches, a 2 KiB history window with 3..34 byte matches, and a 1 KiB history window with 3..66 byte matches. It also tries a repeat-distance LZ variant where explicit matches establish an offset and later matches may reuse that offset with a one-byte token. Linux x86-64 ELF output also evaluates an LZ4-block-style codec with 4-byte matches and a 64 KiB window; it costs a slightly larger stub but can win on static images with repeated regions farther apart than the LZSS windows.

For x86-64 code-heavy images, `expack` also tries a reversible branch transform before LZSS and, under `--all`, XLZ. Relative `call`, `jmp`, and near conditional-branch displacements are converted to file-position targets before compression, and the selected stub converts them back after decompression and before execution. The transform is applied to the whole reconstructed image so the inverse pass restores the same byte positions, including any accidental opcode-looking bytes in data. Linux x86-64 ELF inputs of at least 16 KiB also try `deflate-bcj`, which applies the same branch transform and emits one final dynamic Deflate block. Its ELF runner is dynamic-only to keep the embedded decompressor small, so the host encoder verifies that block shape before accepting the candidate.

Normal pack runs print the input size, detected format, executable-image size, each enabled candidate's payload, stub, and packed estimate, the selected codec, and the final output file size. Use `-q` to suppress that report. Use `--analyze` to inspect the same portfolio decision without writing an output file. By default `expack` evaluates the candidate set that has been useful on the freestanding tool corpus: `lzss/long-match`, `lzrep`, `lzrep-opt`, `lzss-bcj/wide-window`, `lzss-bcj/wide-match`, `lzss-bcj/medium-match`, `lzss-bcj-rip/wide-window`, `lzss-bcj-rip/wide-match`, and `deflate-bcj` for Linux x86-64 ELF images at least 16 KiB. Use `--all` to evaluate every available candidate, including the slower experimental modes and `deflate-bcj` on smaller Linux x86-64 ELF inputs. This is useful when tuning codecs because decoder size is counted in the reported result.

The executable-format layer is intentionally separate from the codec selection and packed-output writer so Mach-O and PE/COFF image backends can be added without rewriting the compression portfolio. Mach-O 64-bit executable inputs can be analyzed and compressed as exact executable images; this keeps code-signature bytes in the payload and avoids rewriting load commands. PE32+ x86-64 inputs can be analyzed with the compression portfolio. Written PE output currently supports LZSS wide-window, LZREP, and x86 branch-transform LZSS payloads inside small no-CRT Windows launchers so the result remains directly runnable.

Mach-O output writes a Mach-O executable-shaped container for an x86-64 or arm64 Mach-O input with a `__TEXT,__expack` section containing an `EXPACKM1` metadata block followed by the payload. For arm64 Mach-O inputs on Darwin, the container can execute LZREP-compressed payloads; its native runner decompresses the exact executable image to a temporary executable file and executes it with the original argument vector and environment. If the selected codec does not have an arm64 Darwin runner yet, the container falls back to a raw exact-image payload. For x86-64 Mach-O inputs, the container is still a signed layout and metadata prototype with a placeholder entry point. The container includes linker-like `__PAGEZERO`, `__TEXT`, `__LINKEDIT`, dyld, build-version, and code-signature load commands. `expack` emits a minimal ad-hoc CodeDirectory signature using SHA-256 page hashes; Apple's `codesign --verify --strict` can be used as an oracle for the generated signature. `--macho-container` remains accepted as an explicit development-mode spelling, but it is no longer required for Mach-O inputs.

PE/COFF output writes a valid PE32+ x86-64 self-extracting executable when a supported compressed payload plus its runner is smaller than the original image. The runner imports only `KERNEL32.dll`, decodes the embedded payload to a temporary executable file, starts it with the current command line, waits for it, returns its exit code, and removes the temporary file. Current PE runners support `lzss/wide-window`, LZREP, and `lzss-bcj/wide-window`. Their embedded templates are generated from the no-CRT source in `src/tools/expack/pe_runner_template.c`; the generator compacts the linked PE headers before writing `pe_runner_template.inc`. If the runner overhead would make the result larger, `expack` writes an exact executable copy instead. This keeps PE output behavior-preserving for the current Windows freestanding tools while avoiding size regressions for tiny binaries.

The compressor can spend extra CPU on optimal match selection because packed binaries are usually produced once and decompressed many times. `expack` ships the decoder that produced the smallest complete packed file, including stub overhead. It is a foundation for future stronger or selectable codecs rather than a UPX-compatible format.

## OPTIONS

| Option | Description |
| --- | --- |
| `-q`, `--quiet` | Do not print the compression-candidate report or size summary. |
| `--all` | Evaluate every available compression candidate instead of the normal reduced portfolio. |
| `--analyze` | Report candidate sizes for `INPUT` without creating a packed executable. |
| `--macho-container` | Explicitly request Mach-O container output; this is the default for Mach-O inputs. |
| `-h`, `--help` | Show usage. |

## LIMITATIONS

- ELF output stubs currently target Linux x86-64 only
- ELF packable inputs must be ELF64 little-endian x86-64 `EXEC` or `DYN` files
- Mach-O 64-bit executable inputs support `--analyze` and default container output; arm64 containers can run LZREP-compressed payloads and use raw payloads as a fallback
- PE32+ x86-64 inputs support default runnable PE/COFF self-extracting output with `lzss/wide-window`, LZREP, and `lzss-bcj/wide-window` payloads when they are smaller than the original; otherwise the output is an exact executable copy. Additional PE decoder codecs are not implemented yet
- the packed executable requires Linux `memfd_create` and `execveat`
- section headers and other bytes not referenced by program headers are not preserved
- load segment file offsets may be canonicalized in the reconstructed image
- this is not a UPX-compatible file format
- the codecs are simple and favor stub size over maximum compression ratio

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

strip, readelf, gzip, xz, project-layout
