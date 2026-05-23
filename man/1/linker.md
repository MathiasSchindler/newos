# LINKER

## NAME

**linker** - link static ELF64 x86-64 executables

## SYNOPSIS

```
linker [-o OUTPUT] [-m elf_x86_64] [--target=TARGET] [options] object-or-archive ...
```

## DESCRIPTION

`linker` combines ELF64 little-endian x86-64 relocatable objects and simple
`ar` archives into a static Linux executable. It is intended as the project
entry point for small freestanding binaries: no dynamic linker, no standard C
library startup files, and no final section-header table are emitted.

The implemented target is intentionally narrow. Inputs must define `_start` or
provide an archive member that defines it. The output is an `ET_EXEC` ELF64 file
for Linux x86-64.

The command-line parser now has an explicit linker target boundary. The supported
implemented target spellings select the existing ELF64 x86-64 backend. Mach-O
arm64 spellings are recognized as a named target, but the backend deliberately
reports that Mach-O arm64 linking is not implemented yet. This keeps future
Darwin work out of the ELF-specific path instead of hiding it behind aliases.

## OPTIONS

- `-o OUTPUT`, `--output=OUTPUT` - write the executable to OUTPUT instead of `a.out`
- `-m elf_x86_64` - select the ELF x86-64 emulation
- `--target=elf64-x86_64`, `--target=x86_64-linux`, `--target=linux-x86_64` - select the implemented ELF64 x86-64 target
- `--target=mach-o-arm64`, `--target=macho64-aarch64`, `--target=macos-aarch64` - select the named Mach-O arm64 target; currently reports an unimplemented backend
- `-e SYMBOL`, `--entry=SYMBOL` - use SYMBOL as the entry symbol instead of `_start`
- `-static`, `--static` - accepted for compatibility; static linking is always used
- `@FILE` - read additional whitespace-separated arguments from FILE; single and double quotes are honored
- `-L DIR` or `-LDIR` - add DIR to the archive library search path
- `-lNAME` - link `libNAME.a` from the configured library search path or current directory
- `--start-group`, `--end-group`, `--whole-archive`, `--no-whole-archive` - accepted for command-line compatibility
- `-h`, `--help` - show usage
- `--lto-cc=COMPILER` - enable transparent LTO prelink support. When any input
  object contains GCC `.gnu.lto_*` sections, the linker invokes COMPILER to run a
  GCC LTO prelink step (`gcc -flto -flinker-output=nolto-rel -r -nostdlib ...`)
  before normal linking. LLVM/Clang bitcode inputs are detected separately and
  ask for `--lto-cc=clang`; for the ELF64 x86-64 backend this routes through a
  Clang/lld relocatable prelink step. In both cases, the resulting native ELF
  relocatable replaces the original LTO IR inputs.

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
written to disk. In `--tiny` mode, segment-internal text, data, and BSS starts
are aligned to the strongest live section requirement instead of a fixed padding
boundary, so post-LTO outputs do not keep unused header or segment gaps when GCC
emits only byte-aligned sections.

## LTO Notes

The linker does not parse compiler IR. It detects LTO containers and asks the
selected compiler driver to lower all inputs into one native relocatable, then
applies the normal newlinker size passes. GCC LTO uses `.gnu.lto_*` section
detection and `-flinker-output=nolto-rel`. LLVM/Clang bitcode detection covers
raw bitcode magic and Mach-O `__LLVM,__bitcode` markers; the current native
prelink implementation is for the ELF64 x86-64 backend and expects a clang/lld
toolchain capable of `-target x86_64-unknown-linux-elf -flto -r`.

Mach-O arm64 LTO remains delegated to Apple clang and Apple ld in the macOS
freestanding-ish build. A project-owned Mach-O arm64 linker backend is a future
target-specific implementation, not an extension of the ELF writer.

Use `make newlinker-lto-size-report` to rebuild both no-LTO and GCC-LTO
freestanding trees and print total size deltas, largest regressions, and largest
wins. Current GCC tuning deliberately keeps the default IPA behavior: tested
`-fno-partial-inlining` and `-fno-ipa-cp-clone` variants did not improve the
overall size result, and `-fno-partial-inlining` made the total output larger.

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

`make macos-freestanding-size-report` prints exact Mach-O file bytes and summed
file-backed Mach-O section bytes for representative macOS freestanding-ish tools.
Use the file-section byte column when judging linker or LTO changes on macOS,
because final Mach-O file sizes can move in coarse 16 KiB-ish steps after page,
segment, and signature layout effects are applied.

`make test-newlinker-optimizations` runs small standalone linker fixtures for
relocation-aware ICF, mergeable string pooling, and reporting output. The ICF
fixture verifies that two relocated functions fold to the same address while the
linked executable still runs.

On Linux x86-64, `make freestanding` is the default newlinker build. It runs
`build-freestanding-newlinker.sh`, writes the canonical freestanding tree under
`build/freestanding-linux-x86_64`, and uses `TARGET_CC` as the object compiler
with this linker for the final links. The script supports both GCC and Clang;
when run directly, set `NEWLINKER_CC` to choose the compiler. Its default
size-focused C flags include `-fmerge-all-constants`; set
`NEWLINKER_EXTRA_CFLAGS` to override or extend the extra compiler flags. The
script compiles with `-fno-stack-protector` and disables the startup stack-guard
initialization call so unused stack-protector support is not retained in tiny
binaries. Set `FREESTANDING_USE_NEWLINKER=0` to use the older system-linker
freestanding Makefile path for comparison.

`make freestanding-newlinker` remains available as an explicit side build under
`build/freestanding-linux-newlinker`.

The build script compiles each needed object once, then links independent tools in
parallel. Set `NEWLINKER_LINK_JOBS=N` to choose the number of simultaneous linker
invocations; when unset, the script uses `PARALLEL_JOBS`, `nproc`, or the online
processor count. Use `NEWLINKER_LINK_JOBS=1` for serial timing or easier log
inspection. The build report records the selected link job count.

Current default `make freestanding` newlinker sizes on this workspace with `cc`
as `TARGET_CC` are: `true` 161 bytes, `false` 162 bytes, `cat` 2279 bytes,
`linker` 26065 bytes, `ncc` 184653 bytes, `ssh` 51621 bytes, and `wget` 68541
bytes. The 185-tool output total is 2420174 bytes.

Current all-tool wall-clock measurements on this workspace: default
`make freestanding` with `cc` as `TARGET_CC` and 16 link jobs took 45.39 seconds.
`make freestanding TARGET_CC=clang` into a separate output tree took 42.41
seconds. In earlier clang-focused timing, `NEWLINKER_LINK_JOBS=1` took 44.29
seconds, the default 16-way link phase took 40.92 seconds, and a fake-linker run
using `LINKER=/usr/bin/true` took 40.35 seconds, so parallel linking removed
almost all linker wall-clock overhead for 185 tools.

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
- LTO bitcode inputs are not parsed directly as native object sections. For GCC
  LTO, pass `--lto-cc=gcc`; for LLVM/Clang bitcode on the ELF64 x86-64 backend,
  pass `--lto-cc=clang` and provide a clang/lld toolchain that can emit a native
  relocatable. Mach-O arm64 LTO is still handled by Apple clang/ld outside this
  linker.
- The Mach-O arm64 target is parsed as a first-class target name, but the actual
  Mach-O object reader, arm64 relocation engine, layout writer, code-signature
  handling, and output writer are not implemented yet.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.
