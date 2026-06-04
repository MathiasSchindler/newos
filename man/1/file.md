# FILE

## NAME

file - determine file type

## SYNOPSIS

```
file [-biv] [--json] [-L|-h] [file ...]
```

## DESCRIPTION

`file` tests each argument and prints a description of its type based on
magic byte patterns and content heuristics. By default each output line contains
the path followed by a compact description. Use `--verbose` when you want the
detected type together with MIME and filesystem metadata.

## CURRENT CAPABILITIES

Recognises the following types by magic bytes or content inspection:

- ELF executable and object files
- ELF class, byte order, object type, and common machine architectures
- 7-Zip, bzip2, xz archives
- Mach-O word size, file type, common CPU families, universal/fat architecture
	counts, and arm64e slices
- PE/COFF (Windows) executables, including PE32/PE32+, common CPU architectures, subsystem, DLL/executable distinction, section count, and verbose header and section-table details
- tar archives, ar static libraries
- Python, Perl, awk, and shell scripts (shebang detection)
- PNG, GIF, BMP, and JPEG images, including dimensions when present in the header
- WAV, WebP, AVI, Ogg, FLAC, MP3, HTML, JSON, XML, SVG, and plain text
- Empty files

## OPTIONS

- `-b`, `--brief` — print only the description or MIME type, without the path prefix
- `-i`, `--mime`, `--mime-type` — print MIME type instead of description
- `-v`, `--verbose` — print a multi-line report with type, MIME, magic label when available, size, mode, inode, device, link count, owner, group, and modification time. Format-specific verbose fields are included when cheaply available, such as PE/COFF timestamp, linker version, entry RVA, image base, image/header size, alignment, checksum, characteristics, DLL hardening flags, and section table.
- `--json` — emit one JSON Lines `file_type` event per input instead of text
- `-L`, `--dereference` — follow symlinks
- `-h`, `--no-dereference` — do not follow symlinks (default)

## LIMITATIONS

- No magic database (`/etc/magic`, `magic.mgc`); recognition is compiled-in.
- Many file types are not recognised and fall back to `application/octet-stream`.
- Image dimensions are decoded only from headers already present in the initial sample; use `imginfo` for deeper image metadata probing.
- No `-z` (decompress before testing).

## EXAMPLES

```
file /bin/ls
file -v /bin/ls
file -b README.md
file -i image.png
file --json build/newlinker-macos-aarch64/true
file --json /usr/bin/true
file -L /proc/self/exe
file *.c
```

## JSON Output

With `--json`, `file` emits one `file_type` event per input using the common
envelope documented in `json-output`. The `data` object includes `path`,
`description`, `mime`, `magic`, `details`, `sampled_bytes`, and, when available,
filesystem metadata such as `size` and `mode`. Symlink events include `target`.
For Mach-O universal binaries, `description` names the slice architectures and
`details` includes the per-slice alignment summary.

Example event:

```json
{"schema":"newos.tool.v1","tool":"file","stream":"stdout","event":"file_type","seq":1,"data":{"path":"true","description":"Mach-O 64-bit executable arm64","mime":"application/x-mach-binary","magic":"Mach-O","details":"","sampled_bytes":4096}}
```

## SEE ALSO

hexdump, strings, stat
