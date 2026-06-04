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
- defaults to static PIE output with section garbage collection and size-oriented optimization
- includes freestanding stack-canary runtime support, with compiler stack-protector instrumentation controlled by `FREESTANDING_STACK_CFLAGS`
- is where Linux ABI, start-up, and libc-dependency mistakes become visible

## WINDOWS FREESTANDING BUILD

The Windows freestanding build is the native PE target without the MSYS POSIX
runtime or the Microsoft C runtime.

- built from PowerShell with `./build-windows-freestanding.ps1`
- writes binaries to `build/freestanding-windows-$(WINDOWS_TARGET_ARCH)/`
- uses the minimal `src/platform/windows/` startup and Kernel32 imports
- currently builds the small text/core tools, comparison/checksum/image/path/filesystem tools, regex/archive/awk/XML groups, native Winsock/TLS-backed `wtf`, and larger bring-up targets including `editor`, `mail`, and `ncc`
- is intentionally separate from the Linux `make freestanding` target while the Windows platform API surface is added incrementally

## MACOS FREESTANDING BUILD

The macOS freestanding build is the native Darwin arm64 project-linker path. It
keeps tool and shared runtime code on the same universal path as the other
targets, compiles Mach-O arm64 objects with Clang, and links final executables
with the in-tree `linker --target=mach-o-arm64` backend.

- built with `make freestanding` on local macOS/aarch64, or explicitly with
  `make macos-newlinker-tools`
- writes binaries to `build/newlinker-macos-aarch64/`
- uses the project `_start` shim, project runtime, and Darwin syscall-backed
  platform layer
- builds the full declared 194-tool macOS surface by default
- emits project-linked Mach-O executables that are intended to have no dylib
  imports and no C standard library dependency
- auto-parallelizes to the host core count when no `-j` option is supplied;
  pass `-jN` or other make jobserver flags to override that behavior
- uses `src/platform/macos/` plus `src/arch/aarch64/macos/` for Darwin-specific
  behavior, with `src/platform/macos/newlinker_start.S` and
  `src/platform/macos/newlinker_runtime.c` providing the project-linked entry and
  runtime additions
- `test-macos-newlinker-tools` builds the same declared set before running
  representative smoke assertions and no-import checks

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
- currently builds the full 194-tool set, including the core/text/filesystem/process set,
  checksums and `bc`, pagers, `wtf`, archive/compression tools, image metadata
  tools, object inspection tools, `awk`, `sql`, `man`, `pstree`, `wget`, `ncc`,
  `netcat`, DNS lookup/query tools, `ssh`, `sshd`, `httpd`, `ping`, `ping6`,
  DHCP probing, `dmesg`, `mknod`, mount/admin command front-ends, read-only
  `ip`, `sh`, `editor`, `mail`, `service`, `make`, and the XML tool family
- uses `src/platform/macos/` plus `src/arch/aarch64/macos/` for Darwin-specific
  behavior
- compiles with freestanding-oriented flags and `-nodefaultlibs -lSystem`, so
  the project code does not call the C standard library even though the binary
  has the unavoidable macOS system ABI dependency
- strips unused sections, local symbols, and Mach-O function-start metadata by
  default; LTO is enabled by default for the macOS libSystem comparison target and
  can be disabled with `MACOS_FREESTANDING_LTO=0`; XML tools and `ncc`
  currently opt out of LTO because they hit Apple-clang LTO-only crashes
- size work on this path should compare pre-raster payload measurements as well
  as final file bytes. `make macos-freestanding-size-report` reports exact file
  bytes and summed file-backed Mach-O section bytes for representative tools, so
  linker or LTO changes can be judged before 16 KiB-ish Mach-O layout/signature
  steps hide smaller gains or regressions. Save a report and rerun with
  `make macos-freestanding-size-compare BASELINE=previous.tsv` to get exact
  file-byte and file-section-byte deltas
- keeps privileged or host-mutating operations conservative on Darwin when the
  project does not yet have a validated macOS implementation, so some admin
  front-ends build and report unsupported operations instead of changing the host

## SELF-HOSTED BUILD

The self-hosted build reuses the in-tree `ncc` compiler for the hosted tool set.

- built with `make selfhost`
- writes binaries to `build/selfhost-<os>-<arch>/`
- uses the hosted `ncc` binary as `CC` while still relying on the system linker
- is the main bootstrap-progress check for Linux/x86-64 today

This path matters especially for shared runtime changes, shell support code,
and anything that adds new low-level dependencies.

## TARGETS

    make               — on macOS build the local hosted set for compatibility symlinks; on Linux build host plus freestanding
    make host          — build the secondary hosted POSIX binaries under build/host-<os>-<arch>/ with compatibility symlinks in build/
    make freestanding  — normal native path: on Linux build the static syscall-only target under build/freestanding-linux-$(TARGET_ARCH)/; on local macOS/aarch64 build the project-linked Mach-O target under build/newlinker-macos-aarch64/
    make freestanding-macos — build the older Apple-ld/libSystem comparison target under build/freestanding-macos-aarch64/
    make macos-newlinker-tools — explicitly build the macOS project-linked Mach-O tool tree under build/newlinker-macos-aarch64/
    make selfhost      — rebuild the hosted binaries with the in-tree ncc under build/selfhost-<os>-<arch>/
    make test          — build host binaries, run smoke/Phase 1 checks, and run the freestanding smoke suite
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
For freestanding Windows PE output, use LLVM/Clang with lld from a regular LLVM
install or from the UCRT64 packages:

  pacman -Syuu
  pacman -S --needed base-devel gcc mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld

Then start the MSYS shell for the hosted build, change to the repository, and
use Clang for the Linux freestanding path:

  cd /c/Users/Mathias\ Schindler/newos
  make host CC=gcc
  make freestanding TARGET_ARCH=x86_64 TARGET_CC=clang

For the Windows freestanding path, MSYS2 is a build convenience rather than a
runtime requirement. The PE binaries do not link against the MSYS POSIX runtime
or a C standard library. A regular LLVM/Clang installation is enough for the
compiler/linker side as long as it can target `x86_64-w64-windows-gnu`; the
PowerShell builder drives `clang.exe` directly and does not require `make` or a
POSIX-style shell as build drivers. From PowerShell, use:

  .\build-windows-freestanding.ps1

The script uses `clang` from `PATH` by default, with common LLVM and MSYS2 Clang
install locations as fallbacks.

`make host` is useful as an early compiler and shell sanity check, but it still
uses the hosted POSIX backend and therefore depends on the MSYS2 POSIX runtime.
To launch those tools directly from PowerShell or `cmd.exe`, keep the MSYS
runtime directory, such as `C:\msys64\usr\bin`, on `PATH`. Do not copy
`msys-2.0.dll` beside the tools: doing so makes the runtime treat the output
directory as its installation root, which breaks paths such as `.` and `/etc`.
In PowerShell, dot-source the helper once per terminal session:

  . .\activate-host-msys.ps1

If local PowerShell execution policy blocks scripts, use the command runner:

  .\run-host-msys.cmd .\build\host-msys-posix-x86_64\ls.exe

`make freestanding` remains the main Makefile target: it builds Linux ABI
binaries using the raw syscall backend under `src/platform/linux/` and
`src/arch/*/linux/`, or the local macOS project-linked Mach-O target on supported
Darwin hosts. `build-windows-freestanding.ps1` is the early native Windows path.
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

## SELF-HOSTED HOST BUILD STATUS

On Linux, the in-tree compiler is now capable enough to rebuild the hosted tool
set in a separate self-host tree.

A typical check looks like:

    make host
    make selfhost

That path now covers the GNU-style Makefile features used by the repository's
normal host build, including includes, conditionals, command-line variable
origin, common text functions, line continuations, pattern rules, and the
manifest extraction pipeline built from `grep -oE` plus `tr`.

This is an important self-hosting milestone, but it is not a full bootstrap
closure yet. The project still relies on the host C compiler, assembler,
linker, and `/bin/sh` to execute the actual compile and link steps.

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
- On macOS, the default `make` behavior still favors local hosted binaries; `make freestanding` on local aarch64 routes to the early Darwin approximation, which still links the required system ABI library
- Windows native hosted binaries are not supported yet; use MSYS2 for hosted POSIX builds and `build-windows-freestanding.ps1` for the native no-CRT PE backend
- There is no install or staging-prefix workflow yet
- Hosted success and freestanding success should be treated as related but separate checks

## SEE ALSO

man, project-layout, compiler, platform, macos, testing
