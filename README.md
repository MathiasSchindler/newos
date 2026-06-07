# newos

newos is an experimental userland project for a Linux-ABI-compatible operating system.

In broad terms, this repository is a growing collection of command-line tools, shell support code, a self-hosting C compiler, shared runtime code, platform backends, and in-tree documentation designed around freestanding builds on macOS and Linux, with hosted POSIX builds kept as a secondary verification and bring-up path.

The project has been written with the help of a finetuned version of GPT 5.4, with an emphasis on portability, small utilities, clear separation between tool logic and platform-specific code, and a freestanding-first design.

## Scope

The repository currently focuses on:

- a broad and growing Unix-style userland of command-line programs
- shell support and shared support code for strings, I/O, archives, hashing, crypto, regex, Unicode text handling, and common tool behavior
- the self-hosting C compiler ncc and its supporting infrastructure
- platform layers for hosted POSIX builds, Linux freestanding targets, and self-hosted rebuilds through ncc where supported
- a small in-tree manual system, with pages stored under [man](man) and viewable through the project's own man tool

## Current status

The normal local workflow on macOS and Linux is now `make freestanding`. The hosted POSIX build remains active for verification, fast iteration, and early platform bring-up. Windows is a new contributor environment for the project; the current practical route is MSYS2 while native Windows platform support is developed.

The userland has expanded substantially across filesystem, text, process, network, archive, build, math, and system-reporting tools. Linux freestanding builds exercise the libc-free syscall path where available, macOS freestanding builds now use the project Mach-O linker and Darwin runtime path, hosted POSIX remains a useful comparison build, and the self-hosted build path through ncc is now a regular bootstrap-progress check.

The repository also includes a lightweight manual browser and a growing set of manual pages for tools, subsystems, and design notes.

## Testing

The repository includes a structured shell-based test suite under [tests](tests).

- the main entry point is `make test`
- Phase 1 per-tool checks live under [tests/phase1](tests/phase1)
- higher-level smoke suites are run by [tests/run_smoke_tests.sh](tests/run_smoke_tests.sh)
- shared helpers live in [tests/lib](tests/lib)
- grouped suites live in [tests/suites](tests/suites)

On Linux, `make test` also exercises representative freestanding binaries. On macOS, freestanding Linux tests are skipped by default, but the normal local build path is still `make freestanding`: on local macOS/aarch64 it builds the project-linked Mach-O target under `build/newlinker-macos-aarch64/`. That path compiles with Clang, links final executables with the in-tree linker, uses the project runtime and `_start`, and is intended to produce no-standard-library, no-dylib-import tools. The current Darwin project-linked surface covers the current 195-tool set: small core, text, metadata, checksum, math, identity, process, terminal, reporting, networking/TLS, filesystem/admin, archive/compression, image metadata, object inspection, USB inspection, SQL, manual, compiler, HTTP, SSH/SCP, DNS, netcat/portscan, DHCP probing, ping/traceroute, WHOIS, read-only IP inspection, shell, editor, mail, service supervision, make, and XML tools. Use `make host` when you specifically want the hosted POSIX comparison build or a quicker bring-up loop for a platform feature that is not native yet. Use `make freestanding-macos` for the older Apple-ld/libSystem comparison build.

On Windows, the freestanding PE output does not depend on MSYS2, a POSIX
runtime, or the Microsoft C runtime. The important build-time tool is a C
compiler/linker that can emit `x86_64-w64-windows-gnu` PE files; LLVM/Clang with
lld is the preferred path. The native PowerShell build path does not invoke
`make`, `sh`, or MSYS2 as build drivers:

```
.\tests\windows\build-windows-freestanding.ps1
```

That script prefers `clang` on `PATH`, then common LLVM/MSYS2 Clang install
locations. MSYS2 can still be used as a convenient way to obtain a packaged
Clang/lld toolchain, but the produced binaries are no-CRT PE files and the build
script drives `clang.exe` directly from PowerShell.

The first useful native/freestanding and POSIX-hosted checks are:

```
make freestanding
make host CC=gcc
```

On Linux x86-64, `make freestanding` builds the canonical freestanding tool tree
with the project ELF linker, parallel link jobs, size-focused compiler flags, and
the raw Linux syscall backend. It honors `TARGET_CC`, so use `TARGET_CC=clang` or
`TARGET_CC=gcc` to choose the object compiler; by default it uses the Makefile's
normal target compiler selection. GCC LTO is enabled by default on this path:
the build script compiles objects with `-flto` and lets the project linker invoke
GCC through `--lto-cc` to lower `.gnu.lto_*` inputs into native ELF before the
final size passes. Use `make newlinker-lto-size-report` to rebuild no-LTO and
GCC-LTO trees side by side and print total deltas plus the largest wins and
regressions. Set `FREESTANDING_USE_NEWLINKER=0` only when you need the older
system-linker freestanding path for comparison. On local macOS/aarch64, `make
freestanding` builds the project-linked Mach-O tool tree under
`build/newlinker-macos-aarch64/` using Clang for object/LTO compilation and the
in-tree linker for final executables. It auto-parallelizes to the host core
count when no `-j` option is supplied.

The explicit macOS/aarch64 project-linker path is also available as:

```
make macos-newlinker-tools
make -j4 test-macos-newlinker-tools
```

The plain `macos-newlinker-tools` target builds every declared macOS freestanding
tool by default; `test-macos-newlinker-tools` builds that set before running the
representative smoke assertions. The path compiles Mach-O arm64
objects with Clang, performs the LTO prelink step with Clang when needed, and
emits the final executable with the in-tree linker. The project-linked macOS
rules pass `MACOS_NEWLINKER_LINK_FLAGS`, defaulting to
`--macho-compact --gc-sections`, so final links use the loader-safe compact
Mach-O load-command policy and ask Clang's LTO prelink step to dead-strip where
it can.
The macOS project runtime keeps its wrappers visible for final symbol resolution
but only force-retains the Darwin/libc-shaped entry points that ld64 needs to
materialize from LTO. Keeping that retention list narrow lets unused runtime
wrappers fall out of small tools instead of pinning them into every binary.
Project-linked Mach-O outputs include `LC_DYLD_INFO_ONLY` rebase metadata,
including real dyld rebase opcodes for remaining 64-bit absolute pointer
relocations.
For size work, `make macos-freestanding-size-report` shows final file bytes,
file-backed section bytes, layout overhead, load-command counts, and top Mach-O
sections. The Mach-O linker's `--map FILE` output can be fed to
`scripts/report-macos-freestanding-size.sh --maps DIR` for build-time input-section and
symbol attribution while keeping final tools symbol-free; pass
`MACOS_NEWLINKER_MAP_DIR=DIR` to the Makefile path to write per-tool maps.
It deliberately treats the resulting binaries as project-linked, no-import
executables: representative smoke tests reject dylib imports. That is stricter
than the explicit `freestanding-macos` comparison build, which still uses
Apple's linker and libSystem. The current project-linked runtime supplies its own
environment handling, page-size `sysconf`, directory enumeration, user/group
lookup, time formatting, network interface queries, and a Darwin syscall-backed
layer for common file, process, terminal, network, and identity entry points.

For `ncc` bootstrap experiments, `scripts/build-freestanding-newlinker.sh` also accepts
`NEWLINKER_CC=build/host-linux-x86_64/ncc NEWLINKER_LTO=1`. In that mode each
tool link is driven through `ncc -flto -nostdlib -static`, using the compiler's
in-tree native ELF linker path. This currently builds the full 195-tool Linux
x86-64 freestanding set and is useful for measuring native `ncc` whole-program
object LTO separately from GCC/Clang LTO.
The native no-CRT Windows PE path is `tests/windows/build-windows-freestanding.ps1`. It now
builds the small text/core tools, comparison/checksum/image/path/filesystem
tools, regex/archive/awk/XML groups, `wtf`, and larger bring-up targets such as
`editor`, `mail`, and the `ncc` compiler executable. `wtf` and `mail` use the
native Winsock/TLS path; certificate validation is not wired to the Windows trust
store yet, so treat network TLS on Windows as bring-up testing rather than a
hardened HTTPS/IMAPS client. The Windows-built `ncc` can target the existing
Linux and macOS backends; emitting Windows PE executables is still future work.
To launch hosted MSYS tools directly from PowerShell or `cmd.exe`, keep the
MSYS runtime on `PATH`, for example `C:\msys64\usr\bin`. Copying
`msys-2.0.dll` beside the tools is not supported because it relocates the MSYS
root away from the installed environment.
In PowerShell, dot-source the helper once per terminal session:

```
. .\tests\windows\activate-host-msys.ps1
```

If local PowerShell execution policy blocks scripts, use the command runner:

```
.\tests\windows\run-host-msys.cmd .\build\host-msys-posix-x86_64\ls.exe
```

## Userland shell

On Linux, `make run-userland` starts an isolated terminal session backed by the freestanding build. It runs `make freestanding` first, then execs the project `env` with an empty environment, `PATH` pointing only at `build/freestanding-linux-$(uname -m)`, `MANPATH` pointing at the repository manuals, and the project `sh` as the shell:

```
make run-userland
scripts/run-userland.sh --no-build
scripts/run-userland.sh -- 'man ls'
```

This does not install files or change the host system; closing the shell returns to the normal Ubuntu environment.

For a smoke test from inside that environment, run:

```
make test
scripts/test.sh --no-build
```

The test enters the freestanding shell with the isolated `PATH` and checks core command lookup, text tools, filesystem operations, archive tools, shell behavior, manuals, SQL, and a bounded local `httpd`/`wget` round trip.

## Documentation

Manual pages live under [man](man), with user-facing commands typically documented in [man/1](man/1) and broader design notes in [man/7](man/7).

The repository also includes its own man program at [src/tools/man.c](src/tools/man.c), so the same tree can provide and browse its own documentation after a build.

Useful design pages include [man/7/foundry.md](man/7/foundry.md), [man/7/userland.md](man/7/userland.md), [man/7/build.md](man/7/build.md), [man/7/testing.md](man/7/testing.md), [man/7/runtime.md](man/7/runtime.md), [man/7/memory.md](man/7/memory.md), and [man/7/unicode.md](man/7/unicode.md).

## Benchmarks

A separate benchmark area now lives under [tests/benchmarks](tests/benchmarks) for host-side performance comparisons against the standard system tools.

You can run it with make benchmark.
