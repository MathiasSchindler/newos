# BUILD

## NAME

build - hosted and freestanding build workflow

## DESCRIPTION

The repository uses one root `Makefile` for the normal freestanding builds and
the secondary hosted POSIX build. The same tools are compiled in multiple modes
where practical, but they are linked against different backends and start-up
paths.

For macOS and Linux work, the normal build is `make freestanding`. On Linux this
builds the libc-free raw-syscall target; on macOS/aarch64 it builds the native
project-linked Mach-O target with Clang for object/LTO compilation and the
in-tree linker for final executables. `make host` is still valuable, but it is
the hosted POSIX verification path and the fast bring-up path for new platforms
or features before native platform code exists.

The project deliberately uses size-oriented executable layouts. On Linux, the
ELF path may omit section headers and pack program headers in ways that remain
loader-compatible but surprise ordinary inspection tools. On macOS, the normal
project-linked path avoids `libSystem` and other dylib imports even though that
is not Apple's recommended model for general applications. The practical success
criterion for these outputs is that the binary performs its job on the target
OS/architecture without crashes and with reasonable resource use. For
inspection, prefer the project tools such as `file`, `readelf`, `objdump`,
`nm`, `size`, and `imgcheck`, which are taught about the project's compact ELF
and Mach-O shapes.

On Windows, the repository is expected to be built from an MSYS2 shell for now.
MSYS2 provides the POSIX shell tools, GNU make, and hosted compiler path, while
the UCRT64 Clang/lld toolchain can build both the existing Linux freestanding
target and the first native Windows freestanding PE executables.

## HOSTED POSIX BUILD

The hosted build is the secondary POSIX verification and bring-up path.

- built with `make host`
- writes binaries to `build/host-<os>-<arch>/`
- keeps compatibility symlinks at `build/<tool>` for the default hosted build
- uses the POSIX backend in `src/platform/posix/`
- is the variant exercised by the smoke tests and benchmarks
- is often the fastest loop for isolated tool, shell, and compiler work

When a native platform implementation is not ready yet, this mode is a useful
way to make progress without blocking the freestanding target design.

## LINUX FREESTANDING BUILD

The Linux freestanding build is the primary Linux target without libc.

- built with `make freestanding`
- writes binaries to `build/freestanding-linux-$(TARGET_ARCH)/`
- uses `src/platform/linux/` plus `src/arch/$(TARGET_ARCH)/linux/`
- links with the minimal `crt0.S` entry path and direct syscalls
- defaults to static ELF output with section garbage collection, safe ICF,
  entry-rooted call-graph ordering, and page-separated RX/RW load segments
- supports opt-in profile-guided final layout: build once with `PROFILE=1
  LINKER_REPORTS=1`, use `profiler --write-call-graph-profile` with the generated
  map and trace, then pass `--call-graph-profile=FILE` in `LINKER_FLAGS` for a
  targeted normal build. Profiles remain opt-in because their quality depends
  on a representative workload.
- omits section headers from final project-linker output; explicit `--tiny`
  builds additionally overlap the final six ELF-header bytes with the first
  program header and may use one RWX segment when writable state is present
- includes freestanding stack-canary runtime support, with compiler stack-protector instrumentation controlled by `FREESTANDING_STACK_CFLAGS`
- is where Linux ABI, start-up, and libc-dependency mistakes become visible

## WINDOWS FREESTANDING BUILD

The Windows freestanding build is the native PE target without the MSYS POSIX
runtime or the Microsoft C runtime.

- built from PowerShell with `./tests/windows/build-windows-freestanding.ps1`
- selects native AArch64 or x86-64 from the host by default
- writes binaries to `build/freestanding-windows-aarch64/` or
  `build/freestanding-windows-x86_64/`
- compiles independent source/profile combinations in parallel, reuses cached
  objects and dependency files across tools and later invocations, and skips
  unchanged final links
- defaults `-Jobs` to the logical processor count and `-LinkJobs` to at most
  four concurrent memory-intensive LTO links; both limits can be overridden
- removes the selected output tree and its `.objects` cache when passed
  `-Clean`
- uses the minimal `src/platform/windows/` startup and Kernel32 imports
- generates architecture-specific import archives with llvm-dlltool, without a
  Windows SDK or MinGW runtime
- links read-only data and imports into the RX text section, applies safe
  identical-code folding, keeps writable data/BSS separate, omits PE
  timestamps, and places architecture stack probes in collectible COMDAT
  sections so they are retained only when referenced
- gives argument-independent `true` and `false` a dedicated startup; their
  AArch64 and x86-64 outputs are 1024-byte, one-section PE files with an
  explicit `ExitProcess` import
- builds every tool declared in `TOOLS` except `ncc` by default; pass
  `-Tools ncc` to build that compiler bring-up target explicitly
- is intentionally separate from the Linux `make freestanding` target while the Windows platform API surface is added incrementally

The in-tree linker also has a direct `--target=pe-arm64` bring-up path. It
accepts raw AArch64 COFF objects and project-owned `.def` files, emits PE32+
imports without an import archive, and supports `--gc-sections`. Section GC
starts at `mainCRTStartup`, follows COFF relocations and associative COMDAT
edges, resolves weak aliases in input order, and emits only imports referenced
by live sections. Import descriptors and the IAT share the RX text output
section, while initialized writable data and BSS remain in a separate RW
section. This keeps the normal 4096-byte section alignment and 512-byte file
alignment while allowing a loader-valid 1024-byte floor for one-section tools.
For example:

```
build\freestanding-windows-aarch64\linker.exe --target=pe-arm64 --gc-sections -o build\tool.exe tool.obj src\platform\windows\imports\kernel32.def
```

Add `--pack` to ask the linker for a self-decompressing ARM64 image:

```
build\freestanding-windows-aarch64\linker.exe --target=pe-arm64 --gc-sections --pack -o build\tool-packed.exe tool.obj src\platform\windows\imports\kernel32.def
```

This mode compresses linked text and initialized data into an RX `.boot`
section and decompresses them directly into a virtual RW `.load` section at
startup. It uses `VirtualProtect` and `FlushInstructionCache` before entering
the original program. It does not create a temporary file or process. When the
complete packed PE is not smaller, the linker emits the ordinary output
byte-for-byte instead.

Run the native ARM64 regression with:

```
.\tests\windows\test-pe-arm64-linker.ps1
```

The direct PE backend does not yet consume archives or LTO objects and does not
emit base-relocation tables. Normal and packed images therefore require the
preferred image base even though Windows ARM64 requires the dynamic-base header
characteristics for these compact images to load. Clang/lld remains the
reference path for general Windows builds while those input and loader features
are completed.

## MACOS FREESTANDING BUILD

The macOS freestanding build is the native Darwin arm64 project-linker path. It
keeps tool and shared runtime code on the same universal path as the other
targets, compiles Mach-O arm64 objects with Clang, and links final executables
with the in-tree `linker --target=mach-o-arm64` backend.

- built with `make freestanding` on local macOS/aarch64
- writes binaries to `build/macos-aarch64/`
- uses the project `_start` shim, project runtime, and Darwin syscall-backed
  platform layer
- builds the declared macOS project-linked tool surface by default
- emits project-linked Mach-O executables that are intended to have no dylib
  imports and no C standard library dependency
- writes `LC_DYLD_INFO_ONLY` rebase metadata: empty when no absolute pointer
  rebases are needed, and populated for 64-bit absolute pointer relocations that
  remain in the linked image
- links with the project-linker flags, which default to
  `--macho-compact --gc-sections`; compact mode keeps loader-safe 16 KiB segment
  alignment while trimming optional load-command payload, and `--gc-sections`
  asks the Clang LTO prelink step to dead-strip when LTO inputs are present
- keeps macOS project-runtime exports visible but force-retains only the
  Darwin/libc-shaped wrappers that ld64 must materialize from LTO; avoid adding
  blanket `used` retention to new runtime wrappers, because that pins unused
  syscall and libc-compatible entry points into otherwise small tools
- auto-parallelizes to the host core count when no `-j` option is supplied;
  pass `-jN` or other make jobserver flags to override that behavior
- uses `src/platform/macos/` plus `src/arch/aarch64/macos/` for Darwin-specific
  behavior, with `src/platform/macos/newlinker_start.S` and
  `src/platform/macos/newlinker_runtime.c` providing the project-linked entry and
  runtime additions
- the macOS freestanding test path builds the same declared set before running
  representative smoke assertions and no-import checks
- `make macos-freestanding-size-report` follows this normal project-linked
  build and reports exact file bytes, summed file-backed Mach-O section bytes,
  raster/layout overhead, load-command counts, and `LC_BUILD_VERSION`
  tool-record counts plus top final sections for representative tools; save a
  report and rerun with `make macos-freestanding-size-compare
  BASELINE=previous.tsv` to get exact file-byte and file-section-byte deltas
- the Mach-O linker supports `--map FILE` for build-time attribution without
  keeping symbols in final executables. For per-tool maps in the Makefile path,
  create a directory and pass `MACOS_MAP_DIR=DIR`; each link writes
  `DIR/TOOL.map`. `scripts/report-macos-freestanding-size.sh --maps DIR` consumes those
  files and appends top input-section and top-symbol contributor columns.

The project-linked runtime supplies environment handling, page-size `sysconf`,
directory enumeration, user/group lookup, time formatting, network interface
queries, and a Darwin syscall-backed layer for common file, process, terminal,
network, and identity entry points. Some privileged or host-mutating operations
remain conservative on Darwin when the project does not yet have a validated
safe implementation.

## MACOS LIBSYSTEM COMPARISON BUILD

The macOS libSystem comparison build is the older native Darwin arm64 step. It keeps
tool and shared runtime code on the same universal path as the other targets,
but uses the system-provided Mach-O entry path and links only `libSystem`, which
is the usual Apple toolchain ABI library for launchable executables.

- built explicitly with `make freestanding-macos`
- writes binaries to `build/freestanding-macos-aarch64/`
- follows the declared macOS comparison tool surface, including the core,
  text, filesystem, process, archive/compression, image/PDF/object inspection,
  XML, networking, shell, compiler, service, and build-support tools that are
  wired for the Apple-ld/`libSystem` comparison path
- uses `src/platform/macos/` plus `src/arch/aarch64/macos/` for Darwin-specific
  behavior
- compiles with freestanding-oriented flags and `-nodefaultlibs -lSystem`, so
  the project code does not call the C standard library even though the binary
  has the unavoidable macOS system ABI dependency
- strips unused sections, local symbols, and Mach-O function-start metadata by
  default; LTO is enabled by default for the macOS libSystem comparison target and
  can be disabled with `MACOS_FREESTANDING_LTO=0`; XML tools and `ncc`
  currently opt out of LTO because they hit Apple-clang LTO-only crashes
- use `make freestanding-macos` for explicit Apple-ld/libSystem size comparisons
  against the project-linked default when needed
- keeps privileged or host-mutating operations conservative on Darwin when the
  project does not yet have a validated macOS implementation, so some admin
  front-ends build and report unsupported operations instead of changing the host

## SELF-HOSTED BUILD

On Linux/x86-64, the self-hosted build uses the hosted `ncc` bootstrap compiler
and the hosted in-tree linker to rebuild the complete tool set as static,
no-libc Linux binaries, including `ncc` and the linker themselves.

- built with `make selfhost`
- writes binaries to `build/selfhost-<os>-<arch>/`
- uses `ncc` for C compilation and the in-tree linker for final executable links
- uses the system assembler for `.S` sources because `ncc` does not yet assemble
  the project startup and syscall files
- starts from host-built `ncc` and linker binaries and uses Bash to orchestrate
  the build
- is the main bootstrap-progress check for Linux/x86-64 today

`make selfhost-hosted` retains the older hosted POSIX rebuild for comparison. It
writes to `build/selfhost-hosted-<os>-<arch>/` and uses the system linker and C
runtime.

This path matters especially for shared runtime changes, shell support code,
and anything that adds new low-level dependencies.

## TARGETS

    make               — on macOS build the local hosted set for compatibility symlinks; on Linux build host plus freestanding
    make host          — build the secondary hosted POSIX binaries under build/host-<os>-<arch>/ with compatibility symlinks in build/
    make freestanding  — normal native path: on Linux build the static syscall-only target under build/freestanding-linux-$(TARGET_ARCH)/; on local macOS/aarch64 build the project-linked Mach-O target under build/macos-aarch64/
    make freestanding-macos — build the older Apple-ld/libSystem comparison target under build/freestanding-macos-aarch64/
    make run-userland  — on Linux build the freestanding tree and start an isolated shell using only those tools
    make selfhost      — rebuild static no-libc Linux/x86-64 binaries with ncc and the in-tree linker under build/selfhost-<os>-<arch>/
    make selfhost-hosted — rebuild hosted POSIX binaries with ncc and the system linker under build/selfhost-hosted-<os>-<arch>/
    make test-selfhost — rebuild and smoke-test the static selfhost tree, then run native compiler regressions through its ncc
    make test          — build host binaries, run smoke/Phase 1 checks, and on Linux run the freestanding and isolated userland smoke suites
    make benchmark     — build host binaries and run tests/benchmarks/run_benchmarks.sh
    make clean         — remove build output

## TARGET SELECTION

`TARGET_ARCH` selects the freestanding arch layer (`x86_64` or `aarch64`). The
default follows the host on Linux and otherwise falls back to `aarch64`.
`TARGET_TRIPLE` is derived from that choice unless it is overridden manually.

Typical examples:

    make freestanding TARGET_ARCH=x86_64
    make freestanding TARGET_ARCH=aarch64

## WINDOWS BOOTSTRAP

Windows support is currently a contributor-environment path, not a native
Windows userland target. For hosted POSIX builds, install the MSYS GCC package.
For freestanding Windows PE output, use LLVM/Clang with lld and llvm-dlltool
from a regular LLVM install. MSYS2 is not required for this path. It remains an
option for the secondary hosted build or as a packaged x86-64 toolchain:

  pacman -Syuu
  pacman -S --needed base-devel gcc mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld

Then start the MSYS shell for the hosted build, change to the repository, and
use Clang for the Linux freestanding path:

  cd /c/Users/Mathias\ Schindler/newos
  make host CC=gcc
  make freestanding TARGET_ARCH=x86_64 TARGET_CC=clang

For the Windows freestanding path, the PE binaries do not link against the MSYS
POSIX runtime or a C standard library. A regular LLVM installation is enough:
the PowerShell builder drives clang, lld, and llvm-dlltool directly and does not
require `make`, a Windows SDK, or a POSIX-style shell. It defaults to
`aarch64-w64-windows-gnu` on ARM64 Windows and `x86_64-w64-windows-gnu` on
x86-64 Windows. From PowerShell, use:

  .\tests\windows\build-windows-freestanding.ps1

The script parallelizes object compilation across the logical processor count
and limits concurrent LTO links to four by default. Override these independently
with `-Jobs N` and `-LinkJobs N`. Objects, generated dependency files, import
archives, and unchanged final links are reused from the selected build directory
on later invocations. Pass `-Clean` to discard that build tree and its cache.

The script uses `clang` from `PATH` by default, with common LLVM and MSYS2 Clang
install locations as fallbacks.

`make host` is useful as an early compiler and shell sanity check, but it still
uses the hosted POSIX backend and therefore depends on the MSYS2 POSIX runtime.
To launch those tools directly from PowerShell or `cmd.exe`, keep the MSYS
runtime directory, such as `C:\msys64\usr\bin`, on `PATH`. Do not copy
`msys-2.0.dll` beside the tools: doing so makes the runtime treat the output
directory as its installation root, which breaks paths such as `.` and `/etc`.
In PowerShell, dot-source the helper once per terminal session:

  . .\tests\windows\activate-host-msys.ps1

If local PowerShell execution policy blocks scripts, use the command runner:

  .\tests\windows\run-host-msys.cmd .\build\host-msys-posix-x86_64\ls.exe

`make freestanding` remains the main Makefile target: it builds Linux ABI
binaries using the raw syscall backend under `src/platform/linux/` and
`src/arch/*/linux/`, or the local macOS project-linked Mach-O target on supported
Darwin hosts. `tests/windows/build-windows-freestanding.ps1` is the early native Windows path.
It links PE executables directly against Kernel32, Ws2_32, and Bcrypt where
needed. The Windows subset now covers the small text/core tools,
comparison/checksum/image, path/filesystem, regex/archive/awk/XML groups, plus
`wtf`, `editor`, `mail`, and the `ncc` compiler executable. The backend has
startup, argument parsing,
stdout/stderr, heap allocation, file read/write/seek, path metadata, environment
lookup, directory create/remove, truncate, flush support, current-directory,
hostname, uname-style queries, basic Winsock, console raw mode, and native TLS
client support. Certificate validation is not yet wired to the Windows trust
store, so TLS tools are useful for bring-up testing but should not be treated as
hardened HTTPS/IMAPS clients. The Windows-built `ncc` can target the existing
Linux and macOS backends; emitting Windows PE executables remains future work.

## COMMON WORKFLOW

A common contributor sequence is:

    make host
    make test
    make selfhost

Use the hosted build for quick iteration. `make test` is the broad regression
gate; it now includes the freestanding smoke suite on platforms where
freestanding Linux builds are available.

## SELF-HOSTED FREESTANDING BUILD STATUS

On Linux/x86-64, the in-tree compiler and linker rebuild all 214 tools in the
current canonical tool set as static no-libc executables, including fresh `ncc`
and linker binaries, in a separate self-host tree. `make test-selfhost` runs the
no-libc smoke suite and native compiler regression suite against that tree.

A typical check looks like:

    make host
    make selfhost

This is an important self-hosting milestone, but it is not a full bootstrap
closure yet. Host-built `ncc` and linker binaries start the self-host stage, and
the build still relies on Bash and the system assembler. The produced tool
binaries do not depend on the system C library or system linker.

## CONTRIBUTOR NOTES

- Most new tools only need `src/tools/name.c` plus an entry in `TOOLS` in the
  `Makefile`; the generic pattern rules handle both build variants.
- If a tool grows private helper files or internal headers, keep the public
  entry point at `src/tools/name.c` and place the private implementation under
  `src/tools/name/`.
- Reserve `src/shared/` for code that is genuinely reused across multiple tools.
- `sh` and `ncc` have explicit rules because they pull in additional private or
  shared subsystems. A new tool with special dependencies should follow that
  pattern.
- Hosted, freestanding, and self-hosted outputs are intentionally separate. A
  passing hosted build does not guarantee the syscall-only target is also
  healthy.
- If a new shared runtime helper is added, make sure any explicit special-case
  build rules and `src/compiler/source_manifest.h` stay in sync; the Makefile
  derives several source groups from that manifest.
- Header dependency tracking is lightweight. If shared headers or build flags
  change, prefer `make clean && make host`.

## LIMITATIONS

- The Linux freestanding build currently assumes a compiler/linker combination capable of `-nostdlib` static PIE output, normally Clang plus `lld`
- On macOS/aarch64, `make freestanding` is the project-linked Mach-O path; use `make freestanding-macos` only for the older Apple-ld/libSystem comparison build
- Windows native hosted binaries are not supported yet; use MSYS2 for hosted POSIX builds and `tests/windows/build-windows-freestanding.ps1` for the native no-CRT PE backend
- There is no install or staging-prefix workflow yet
- Hosted success and freestanding success should be treated as related but separate checks

## SEE ALSO

man, project-layout, foundry, compiler, platform, macos, windows, testing
