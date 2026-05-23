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
  relocation records, and exact-size sections whose relocation records refer to
  equivalent targets. Conservative suffix folding is still limited to
  relocation-free read-only data.

The linker also coalesces eligible ELF mergeable byte-string sections
(`SHF_MERGE | SHF_STRINGS`, entry size 1) into one string pool. Duplicate strings
and strings that are suffixes of longer strings share the same output bytes while
relocations are retargeted to the pooled offset. This primarily reduces tools
with many diagnostics or compiler tables, such as `ncc`.

The linker orders live sections by alignment and places zero-tailed writable data
late in the data image so padding and trailing zero bytes are less likely to be
written to disk.

## Reporting Options

- `--stats` - print link statistics to standard output, including live object and
  section counts, relocation count, folded/discarded bytes, text/data/BSS sizes,
  file size, memory size, header bytes, padding bytes, and active policy.
- `--map FILE` - write a map file listing live sections, output virtual
  addresses, sizes, source objects, and the reason each section became live.
- `--why-live SYMBOL_OR_SECTION` - print why a specific live symbol or section is
  present in the output, including the relocation chain back toward the entry
  section when section GC is active.
- `--print-gc-sections` - print allocatable sections discarded by section GC.

## EXAMPLES

```
linker -o hello crt0.o hello.o runtime.a
linker -m elf_x86_64 -o app start.o main.o
linker --tiny --gc-sections --stats -o true crt0.o true.o runtime.o
linker --tiny --gc-sections --print-gc-sections -o true crt0.o true.o runtime.o
linker --tiny --gc-sections --map app.map --why-live main -o app @objects.rsp
```

## TESTING

`make test-newlinker-expack` packs every tool from the newlinker freestanding
tree with `expack --all`, using `PARALLEL_JOBS` jobs by default. Packed
executables are written to `build/freestanding-linux-expack` with the same names
as the input tools. The test also smoke-tests the packed `true`, `false`, and
`cat` binaries. Set `NEWOS_NEWLINKER_BUILD_DIR` to point at a non-default
newlinker build tree, or `NEWOS_EXPACK_FLAGS` to override the default `--all`
packing mode.

`make newlinker-size-report` builds a report-enabled newlinker tree and prints
file size, traditional-baseline size, delta, text, data, BSS, folded-byte,
discarded-byte, top-section, and top-object size attribution for representative
tools. Top-section and top-object lists focus on file-backed payload by omitting
BSS and already folded sections. The report target is intended to guide
feature-preserving size work.

`make test-newlinker-optimizations` runs small standalone linker fixtures for
relocation-aware ICF, mergeable string pooling, and reporting output. The ICF
fixture verifies that two relocated functions fold to the same address while the
linked executable still runs.

`make freestanding-newlinker` runs `build-freestanding-newlinker.sh` to build the
freestanding Linux x86-64 tool tree with this linker. Its default size-focused C
flags include
`-fmerge-all-constants`; set `NEWLINKER_EXTRA_CFLAGS` to override or extend the
extra compiler flags. The script compiles with `-fno-stack-protector` and disables
the startup stack-guard initialization call so unused stack-protector support is
not retained in tiny binaries.

Current measured newlinker sizes on this workspace are: `true` 157 bytes,
`false` 158 bytes, `cat` 2159 bytes, and `ncc` 195565 bytes for focused report
builds after relocation-aware ICF and embedded-linker reporting removal. Against
the traditional freestanding tree, 184 of 185 tools are smaller; `ncc` remains
the only larger binary.

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
- `--icf=safe` relocation-aware folding is intentionally conservative: relocated
  sections fold only when relocation offsets, types, addends, and target symbols
  are equivalent. General equivalence-class ICF for mutually recursive local
  sections is not implemented yet.
- Mergeable string pooling is limited to allocatable read-only byte-string
  sections with no relocations in the string section itself. Other `SHF_MERGE`
  entry sizes are implemented as an experimental build-time constant-pooling
  path, but are disabled by default until every relocation pattern is proven
  safe. Relocation-bearing merge sections are kept on the normal section path.
- x86-64 size-changing instruction relaxation is not implemented yet. The
  current freestanding `ncc` inputs do not expose common same-size GOT relaxation
  relocations; real byte savings would require rewriting section contents,
  symbol values, relocation offsets, and layout-dependent references.
- Archives are parsed by this linker directly; archive symbol indexes are not
  required.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.
