# LINKER

## NAME

**linker** - link static ELF64 x86-64 executables

## SYNOPSIS

```
linker [-o OUTPUT] [-m elf_x86_64] [options] object-or-archive ...
```

## DESCRIPTION

`linker` combines ELF64 little-endian x86-64 relocatable objects and simple
`ar` archives into a static Linux executable. It is intended as the project
entry point for small freestanding binaries: no dynamic linker, no standard C
library startup files, and no final section-header table are emitted.

The initial target is intentionally narrow. Inputs must define `_start` or
provide an archive member that defines it. The output is an `ET_EXEC` ELF64 file
for Linux x86-64.

## OPTIONS

- `-o OUTPUT`, `--output=OUTPUT` - write the executable to OUTPUT instead of `a.out`
- `-m elf_x86_64` - select the ELF x86-64 emulation
- `--target=elf64-x86_64` - select the ELF64 x86-64 target
- `-e SYMBOL`, `--entry=SYMBOL` - use SYMBOL as the entry symbol instead of `_start`
- `-static`, `--static` - accepted for compatibility; static linking is always used
- `@FILE` - read additional whitespace-separated arguments from FILE; single and double quotes are honored
- `-L DIR` or `-LDIR` - add DIR to the archive library search path
- `-lNAME` - link `libNAME.a` from the configured library search path or current directory
- `--start-group`, `--end-group`, `--whole-archive`, `--no-whole-archive` - accepted for command-line compatibility
- `-h`, `--help` - show usage

## Size and Layout Options

- `--tiny`, `--pack-segments` - emit a packed executable layout with one compact
  load segment. This minimizes file size by avoiding 4 KiB segment padding. If
  writable data or BSS is present, the segment is readable, writable, and
  executable. Trailing zero bytes in the load image may be omitted from the file
  and represented by the ELF segment memory size instead.
- `--separate-code`, `--page-align` - use the default page-aligned RX/RW segment
  layout.
- `--gc-sections` - keep only sections reachable from the entry symbol through
  relocations. This is most useful with compiler inputs built using
  `-ffunction-sections -fdata-sections`.
- `--no-gc-sections` - disable section-level garbage collection and keep the
  current object-level reachability behavior.
- `--icf=safe`, `--icf` - fold identical live read-only sections that have no
  relocation records. This is a conservative subset of identical code/data
  folding.

## Reporting Options

- `--stats` - print link statistics to standard output, including live object and
  section counts, relocation count, text/data/BSS sizes, file size, memory size,
  header bytes, padding bytes, and active policy.
- `--map FILE` - write a map file listing live sections, output virtual
  addresses, sizes, source objects, and the reason each section became live.
- `--why-live SYMBOL_OR_SECTION` - print why a specific live symbol or section is
  present in the output.

## EXAMPLES

```
linker -o hello crt0.o hello.o runtime.a
linker -m elf_x86_64 -o app start.o main.o
linker --tiny --gc-sections --stats -o true crt0.o true.o runtime.o
linker --tiny --gc-sections --map app.map --why-live main -o app @objects.rsp
```

## TESTING

`make test-newlinker-expack` packs representative newlinker-produced ELF files
with `expack` and smoke-tests the packed `true`, `false`, and `cat` binaries.
Set `NEWOS_NEWLINKER_BUILD_DIR` to point at a non-default newlinker build tree.

## LIMITATIONS

- Only ELF64 little-endian x86-64 inputs are supported.
- The output format is currently Linux `ET_EXEC` only.
- Dynamic linking, shared libraries, linker scripts, symbol versioning, TLS,
  debug-info preservation, and PIE output are not implemented.
- Relocation support is limited to the relocation types used by the current
  freestanding path: absolute 64-bit, PC-relative 32-bit, PLT32 calls, and
  absolute 32-bit/32-bit signed references.
- `--tiny` trades strict W^X separation for smaller files when writable memory is
  present. Use the default page-aligned layout when separate executable and
  writable mappings are required.
- `--icf=safe` currently folds only read-only sections with no relocation
  records. More aggressive relocation-aware folding is not implemented yet.
- Archives are parsed by this linker directly; archive symbol indexes are not
  required.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.
