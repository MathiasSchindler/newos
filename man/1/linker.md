# LINKER

## NAME

**linker** - link static ELF64 x86-64 and project Mach-O arm64 executables

## SYNOPSIS

```
linker [-o OUTPUT] [-m elf_x86_64] [--target=TARGET] [options] object-or-archive ...
```

## DESCRIPTION

`linker` combines ELF64 little-endian x86-64 relocatable objects and simple
`ar` archives into a static Linux executable. It also owns the Mach-O arm64
backend for the macOS project-linked freestanding build. It is intended as the
project entry point for small freestanding binaries: no standard C library
startup files are required, the ELF backend emits no dynamic linker or final
section-header table, and the Mach-O backend writes ad-hoc signed executables
with no intended dylib imports.

The implemented targets are intentionally narrow. Inputs must define `_start` or
provide an archive member that defines it. Linux output is an `ET_EXEC` ELF64
file for x86-64; macOS output is a loader-safe arm64 Mach-O executable for the
project runtime and Darwin syscall layer.

The command-line parser has an explicit linker target boundary. The supported
ELF spellings select the existing ELF64 x86-64 backend. Mach-O arm64 spellings
select the Darwin backend used by `make freestanding` on local macOS/aarch64.
This keeps Darwin work in a target-specific path instead of hiding it behind ELF
aliases.

## OPTIONS

- `-o OUTPUT`, `--output=OUTPUT` - write the executable to OUTPUT instead of `a.out`
- `-m elf_x86_64` - select the ELF x86-64 emulation
- `--target=elf64-x86_64`, `--target=x86_64-linux`, `--target=linux-x86_64` - select the implemented ELF64 x86-64 target
- `--target=mach-o-arm64`, `--target=macho64-aarch64`, `--target=macos-aarch64` - select the early Mach-O arm64 target
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
  Clang/lld relocatable prelink step. For the Mach-O arm64 backend, Apple clang
  and ld64 are used to materialize the native LTO object with
  `-Wl,-object_path_lto`. In all cases, the resulting native relocatable replaces
  the original LTO IR inputs before target-specific linking continues.

## Size and Layout Options

- `--tiny`, `--pack-segments` - emit a packed executable layout with one compact
  load segment. This minimizes file size by avoiding 4 KiB segment padding. If
  writable data or BSS is present, the segment is readable, writable, and
  executable. Trailing zero bytes in the load image may be omitted from the file
  and represented by the ELF segment memory size instead. On Mach-O arm64 this
  selects the compact load-command policy described below.
- `--separate-code`, `--page-align` - use the default page-aligned RX/RW segment
  layout. This is the Linux `make freestanding` default. Binaries with writable
  data receive distinct executable/non-writable and writable/non-executable
  load segments.
- `--gc-sections` - keep only sections reachable from the entry symbol through
  relocations. This is most useful with compiler inputs built using
  `-ffunction-sections -fdata-sections`. On Mach-O arm64, the final backend is
  still conservative after Clang LTO materializes one native object, but this
  option asks the Clang/ld64 LTO prelink step to use `-dead_strip`. The macOS
  project runtime depends on narrow `used` annotations for the Darwin/libc-shaped
  wrappers ld64 must materialize; broad annotations defeat this dead-strip pass
  and keep unused runtime wrappers in small tools.
- `--no-gc-sections` - disable section-level garbage collection and keep the
  current object-level reachability behavior.
- `--macho-compact` - for Mach-O arm64 outputs, preserve loader-safe 16 KiB
  page-aligned segment layout while omitting the optional `LC_BUILD_VERSION`
  tool record. This saves load-command payload bytes and can reduce file size
  when a binary is near a page/signature threshold. It deliberately keeps
  `__PAGEZERO`, because omitting it produced kernel-rejected executables in
  testing.
- `--icf=safe`, `--icf` - fold identical live read-only sections that have no
  relocation records, and exact-size sections whose relocation records refer to
  equivalent targets. Conservative suffix folding is still limited to
  relocation-free read-only data.
- `--icf=all` - additionally use iterative equivalence-class refinement for
  executable sections. Relocation targets are compared by their current class,
  allowing structurally identical mutually recursive local function groups to
  fold. This mode may make distinct function addresses equal and is therefore
  explicit rather than part of `--icf=safe`.
- `--call-graph-order` - order live executable sections from the entry section
  through relocation edges before applying the deterministic alignment/size
  fallback. Edges to folded functions follow their canonical ICF master. This
  keeps entry-path callers and callees close without requiring profile data.
- `--symbol-ordering-file FILE` - place live executable sections named by FILE
  first, one symbol or section name per line. Blank lines and `#` comments are
  ignored. Earlier names win, names absent from the current link are ignored,
  and the normal call-graph/alignment policy orders everything not named.
- `--call-graph-profile FILE` - apply weighted profile-guided function ordering.
  The stable text format contains `node TOTAL_NS SYMBOL` and
  `edge CALLS CALLER CALLEE` records. The linker starts with the hottest
  unplaced node, follows its hottest unplaced outgoing edge, and repeats. Use
  `profiler --write-call-graph-profile` to generate the file. Explicit symbol
  ordering has first priority, the weighted profile follows, and
  `--call-graph-order` supplies the deterministic fallback. Unknown symbols are
  ignored so profiles tolerate source changes; malformed records are errors.
- `--merge-constants` - coalesce duplicate entries in eligible non-string ELF
  `SHF_MERGE` sections. Input-section alignment and output-pool alignment are
  tracked separately, and unsupported relocation patterns leave a section on
  the normal path. The option is not enabled by default; see the measured result
  below.

The linker also coalesces eligible ELF mergeable byte-string sections
(`SHF_MERGE | SHF_STRINGS`, entry size 1) into one string pool. Duplicate strings
and strings that are suffixes of longer strings share the same output bytes while
relocations are retargeted to the pooled offset. This primarily reduces tools
with many diagnostics or compiler tables, such as `ncc`.

Without `--call-graph-order`, the linker orders live sections by alignment and
size. With call-graph ordering, entry-reachable executable sections are placed
first in relocation traversal order and the same alignment/size policy handles
the remainder. Zero-tailed writable data is placed late so padding and trailing
zero bytes are less likely to be written to disk. In `--tiny` mode,
segment-internal text, data, and BSS starts are aligned to the strongest live
section requirement instead of a fixed padding boundary.

## LTO Notes

The linker does not parse compiler IR. It detects LTO containers and asks the
selected compiler driver to lower all inputs into one native relocatable, then
applies the normal newlinker target pass. GCC LTO uses `.gnu.lto_*` section
detection and `-flinker-output=nolto-rel`. LLVM/Clang bitcode detection covers
raw bitcode magic and Mach-O `__LLVM,__bitcode` markers. The ELF64 x86-64 path
expects a clang/lld toolchain capable of
`-target x86_64-unknown-linux-elf -flto -r`. The Mach-O arm64 path uses Apple
clang/ld64 with `-Wl,-object_path_lto` and then links the native object itself.

`make test-linker-cli` includes an optional Clang LTO fixture. It compiles a tiny
raw LLVM bitcode object, probes whether the selected clang can prelink x86-64 ELF
with lld, and then asks this linker to produce an ELF executable via
`--lto-cc=clang`. The fixture skips cleanly when that cross-LTO toolchain is not
available.

The project-owned Mach-O backend can now consume tiny syscall-only clang C
objects directly, including split source files and simple archives. It can also
consume clang LTO bitcode when `--lto-cc=clang` is provided. The LTO lowering
step uses Apple ld64's `object_path_lto` output as the native relocatable and
then this backend writes the final ad-hoc signed Mach-O executable itself.

## Mach-O arm64 Subset

The Mach-O backend accepts multiple arm64 relocatable objects and simple
archives. One input must contain `_start` or the symbol named with `-e` in
`__TEXT,__text`; Darwin C entry symbols are represented with the Mach-O symbol
spelling `__start`, and the backend accepts that symbol when the requested entry
is `_start`. Supported payload sections are `__TEXT,__text`, `__TEXT,__const`,
`__TEXT,__cstring`, `__DATA,__data`, and zero-fill `__DATA,__bss`. The linker
groups same-kind sections into one output section per kind and resolves external
defined symbols across input objects. The relocation pass supports the arm64
`BRANCH26`, `PAGE21`, `PAGEOFF12`, and simple absolute `UNSIGNED` records needed
by local and cross-object calls, `adrp`/`add` references, and clang's
unsigned-offset load/store references to static data. The writer emits
`__PAGEZERO`, `__TEXT`, optional `__DATA`, `__LINKEDIT`, `LC_DYLD_INFO_ONLY`,
`LC_LOAD_DYLINKER`, `LC_BUILD_VERSION`, `LC_MAIN`, and `LC_CODE_SIGNATURE`. With
`--macho-compact`, the `LC_BUILD_VERSION` command uses `ntools 0` and omits the
tool-version payload. PIE outputs include empty dyld rebase metadata when no
absolute pointer rebases are needed, and classic dyld rebase opcodes for 64-bit
absolute pointer relocations that remain in the linked image. Output uses the
same 16 KiB page/signature granularity as the macOS prototype container writer
and includes an in-tree ad-hoc SHA-256 CodeDirectory signature.

This is enough for tiny syscall-only arm64 start objects and freestanding clang
C start files split across several translation units, with local calls,
cross-object calls, literal strings, static const data, initialized data,
zero-initialized data, archive members, and the same shape after clang LTO
prelinking. General Darwin linking still needs selective archive member loading,
common-symbol handling, more section classes, and the rest of the arm64
relocation vocabulary. The backend is aimed at the project's no-libSystem
runtime rather than arbitrary Apple SDK applications.

Use `make newlinker-lto-size-report` to rebuild both no-LTO and GCC-LTO
freestanding trees and print total size deltas, largest regressions, and largest
wins. Current GCC tuning deliberately keeps the default IPA behavior: tested
`-fno-partial-inlining` and `-fno-ipa-cp-clone` variants did not improve the
overall size result, and `-fno-partial-inlining` made the total output larger.

## Reporting Options

- `--stats` - print link statistics to standard output. The ELF backend reports
  live object and section counts, relocation count, folded/discarded bytes,
  GC bytes by section class, exact/suffix/equivalence ICF counts and bytes,
  string and constant pool input/output/savings, text/data/BSS sizes, file size,
  memory size, header and padding bytes, ordered-section count, profile
  node/edge coverage and ordered payload, RX page slack and bytes needed to
  remove the next W^X page, executable-suffix candidates, relaxable x86
  cross-section jumps, segment permissions, and active policy. The Mach-O backend reports input object/section counts,
  file-backed section payloads, BSS bytes, header bytes, segment file and VM
  sizes, signature offset/code limit/signature bytes, final file size, and the
  active page-aligned or compact policy.
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
linker --separate-code --gc-sections --icf=safe --call-graph-order -o app @objects.rsp
linker --separate-code --gc-sections --symbol-ordering-file hot.order -o app @objects.rsp
linker --separate-code --gc-sections --call-graph-profile app.cgprofile --call-graph-order -o app @objects.rsp
linker --tiny --gc-sections --icf=all --merge-constants --stats -o app @objects.rsp
linker --target=mach-o-arm64 --macho-compact --gc-sections --lto-cc=clang -o app start.o app.o runtime.o
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

`make macos-freestanding-size-report` prints exact Mach-O file bytes, summed
file-backed Mach-O section bytes, raster/layout overhead, load-command counts,
`LC_BUILD_VERSION` tool-record counts, and top file-backed Mach-O sections for
representative project-linked macOS freestanding tools. Use the file-section
byte column when judging linker or LTO changes on macOS, because final Mach-O
file sizes can move in coarse 16 KiB-ish steps after page, segment, and
signature layout effects are applied.

The Mach-O backend also honors `--map FILE`. Its map records final sections,
input sections, and symbol-size attribution seen during linking; the final
executables still omit `LC_SYMTAB`. For Makefile builds, create a directory and
pass `MACOS_NEWLINKER_MAP_DIR=DIR` to write `DIR/TOOL.map` files. Pass
`--maps DIR` to `scripts/report-macos-freestanding-size.sh`, and the report appends top
input-section and top-symbol contributors. Without maps, those attribution
columns are reported as unavailable.

Save a report and compare later with `make macos-freestanding-size-compare
BASELINE=previous.tsv`. The compare output adds `delta_file_bytes` and
`delta_file_section_bytes`; the latter is the main pre-raster signal for macOS
size work. The report target follows the normal macOS `make freestanding`
default and measures `build/newlinker-macos-aarch64/` unless the build directory
is overridden.

`tests/suites/newlinker_optimizations.sh` runs standalone fixtures for
relocation-aware safe ICF, mutually recursive equivalence-class ICF, entry-rooted
call-graph, explicit-symbol, and weighted-profile ordering, mergeable string and
constant pooling, detailed reporting, optimization candidate censuses, tiny
layout, and LTO. The freestanding smoke suite checks every produced ELF for
writable-executable load segments and verifies distinct RX/RW mappings on a tool
with writable state.

The candidate counters are intentionally diagnostic rather than rewriting
passes. A July 2026 report-enabled build of all 214 canonical Linux tools found
307 short-jump sites worth 921 payload bytes across 165 tools and 49
relocation-aware executable-suffix candidates worth 114 payload bytes across
39 tools. Neither class, alone or combined, crossed the reported W^X page
threshold for a single output. Variable-length instruction rewriting and
executable tail folding therefore remain deferred until measurements show a
final-file or runtime win that justifies their address, relocation, and
function-identity risk.

On Linux x86-64, `make freestanding` is the default newlinker build. It runs
`scripts/build-freestanding-newlinker.sh`, writes the canonical freestanding tree under
`build/freestanding-linux-x86_64`, and uses `TARGET_CC` as the object compiler
with this linker for the final links. Final links default to `--separate-code
--gc-sections --icf=safe --call-graph-order`, so writable tools use W^X load
segments. Pass `LINKER_FLAGS` explicitly when producing minimum-size `--tiny`
artifacts. The script supports both GCC and Clang;
when run directly, set `NEWLINKER_CC` to choose the compiler. Its default
size-focused C flags include `-fmerge-all-constants`; set
`NEWLINKER_EXTRA_CFLAGS` to override or extend the extra compiler flags. The
script compiles with `-fno-stack-protector` and disables the startup stack-guard
initialization call so unused stack-protector support is not retained in tiny
binaries. Set `FREESTANDING_USE_NEWLINKER=0` to use the older system-linker
freestanding Makefile path for comparison.

The build script compiles each needed object once, then links independent tools in
parallel. Set `NEWLINKER_LINK_JOBS=N` to choose the number of simultaneous linker
invocations; when unset, the script uses `PARALLEL_JOBS`, `nproc`, or the online
processor count. Use `NEWLINKER_LINK_JOBS=1` for serial timing or easier log
inspection. The build report records the selected link job count.

Constant merging was measured on the complete 214-tool hardened Linux build on
2026-07-11. It removed 464 bytes of file-backed section payload across three
tools (`solve` 368, `printf` 64, and `git` 32), but page-separated segment
layout absorbed all of that space: aggregate final output remained 4,448,854
bytes and no individual file changed size. The full freestanding smoke suite
passed with the option enabled. Consequently `--merge-constants` remains an
available measured optimization rather than a default build flag.

## LIMITATIONS

- ELF input support is limited to ELF64 little-endian x86-64 relocatables and
  archives. Mach-O input support is limited to arm64 relocatables and simple
  archives in the section/relocation subset used by the project.
- Dynamic linking, shared libraries, linker scripts, symbol versioning,
  debug-info preservation, and arbitrary SDK-style Darwin linking are not
  implemented. The Mach-O backend does emit PIE-shaped project executables for
  the macOS no-libSystem path.
- Relocation support is limited to the relocation types used by the current
  freestanding path: absolute 64-bit, PC-relative 32-bit, PLT32 calls, and
  absolute 32-bit/32-bit signed references.
- `--tiny` trades strict W^X separation for smaller files when writable memory is
  present. Use the default page-aligned layout when separate executable and
  writable mappings are required.
- `--icf=safe` relocation-aware folding is intentionally conservative: relocated
  sections fold only when relocation offsets, types, addends, and target symbols
  are equivalent. `--icf=all` supports equivalence-class folding for mutually
  recursive executable sections but is address-significant and intentionally
  opt-in.
- Mergeable string pooling is limited to allocatable read-only byte-string
  sections with no relocations in the string section itself. Other `SHF_MERGE`
  entry sizes are available through `--merge-constants` when their symbols and
  relocation references are entry-aligned and translatable. Relocation-bearing
  merge sections and unsupported reference patterns remain on the normal path.
- x86-64 size-changing instruction relaxation is not implemented. `--stats`
  counts cross-section `jmp rel32` sites whose final displacement fits `rel8`,
  and also reports relocation-aware executable suffix-fold candidates. Current
  corpus measurements do not recover a W^X page from either transformation;
  implementing them would require rewriting section contents, symbol values,
  relocation offsets, and layout-dependent references for no final-file win.
- Archives are parsed by this linker directly; archive symbol indexes are not
  required.
- LTO bitcode inputs are not parsed directly as native object sections. For GCC
  LTO, pass `--lto-cc=gcc`; for LLVM/Clang bitcode on the ELF64 x86-64 backend,
  pass `--lto-cc=clang` and provide a clang/lld toolchain that can emit a native
  relocatable. For Mach-O arm64 clang LTO, pass `--lto-cc=clang`; Apple clang and
  ld64 materialize the native LTO object and this linker writes the final Mach-O.
- The Mach-O arm64 target is first-class but intentionally tiny: archives are
  loaded as simple object containers rather than via selective symbol-index
  extraction, common symbols are not implemented, supported sections are limited
  to the current text/const/cstring/data/bss set, and the arm64 relocation
  vocabulary is still the subset used by the current clang syscall fixtures.

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.
