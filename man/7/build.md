# BUILD

## NAME

build - hosted and freestanding build workflow

## DESCRIPTION

The repository uses one root `Makefile` for both normal development and the
freestanding target. The same tools are compiled in both modes where practical,
but they are linked against different backends and start-up paths.

For most contributors, the usual loop is `make host` followed by `make test`.
On Linux, `make test` also builds and exercises the libc-free freestanding
target so syscall-only regressions do not wait for a separate manual check. On
macOS, the local default remains the hosted build unless an explicit
alternative target is requested.

On Windows, the repository is expected to be built from an MSYS2 shell for now.
MSYS2 provides the POSIX shell tools, GNU make, and hosted compiler path, while
the UCRT64 Clang/lld toolchain can build both the existing Linux freestanding
target and the first native Windows freestanding PE executables.

## HOSTED BUILD

The hosted build is the normal development path.

- built with `make host`
- writes binaries to `build/host-<os>-<arch>/`
- keeps compatibility symlinks at `build/<tool>` for the default hosted build
- uses the POSIX backend in `src/platform/posix/`
- is the variant exercised by the smoke tests and benchmarks
- is the fastest loop for day-to-day tool, shell, and compiler work

When you are changing visible behavior or adding a new utility, this is usually
the first mode to get green.

## FREESTANDING BUILD

The freestanding build is the Linux target without libc.

- built with `make freestanding`
- writes binaries to `build/freestanding-linux-$(TARGET_ARCH)/`
- uses `src/platform/linux/` plus `src/arch/$(TARGET_ARCH)/linux/`
- links with the minimal `crt0.S` entry path and direct syscalls
- defaults to static PIE output with section garbage collection and size-oriented optimization
- includes freestanding stack-canary runtime support, with compiler stack-protector instrumentation controlled by `FREESTANDING_STACK_CFLAGS`
- is where ABI, start-up, and portability mistakes become visible

## WINDOWS FREESTANDING BUILD

The Windows freestanding build is the native PE target without the MSYS POSIX
runtime or the Microsoft C runtime.

- built with `make freestanding-windows WINDOWS_TARGET_CC=clang`
- writes binaries to `build/freestanding-windows-$(WINDOWS_TARGET_ARCH)/`
- uses the minimal `src/platform/windows/` startup and Kernel32 imports
- currently builds the small tool subset `true`, `false`, `echo`, `printf`, `dirname`, `basename`, `cat`, `head`, `tail`, `nl`, `rev`, `fold`, `uniq`, `wc`, `cut`, `tr`, `expand`, `unexpand`, `pwd`, `hostname`, and `uname`, plus comparison, checksum, image, path, and basic filesystem tools including `cmp`, `comm`, `join`, `paste`, `tac`, `sleep`, `file`, `readlink`, `realpath`, `strings`, `hexdump`, `od`, `md5sum`, `sha256sum`, `sha512sum`, `test`, `which`, `printenv`, `tee`, `mkdir`, `rmdir`, `truncate`, `sync`, `imgmeta`, `imginfo`, `imgcheck`, `bc`, `expr`, and `seq`, plus native Winsock/TLS-backed `wtf`
- is intentionally separate from the Linux `make freestanding` target while the Windows platform API surface is added incrementally

## MACOS FREESTANDING-ISH BUILD

The macOS freestanding-ish build is the first native Darwin arm64 step. It keeps
tool and shared runtime code on the same universal path as the other targets,
but uses the system-provided Mach-O entry path and links only `libSystem`, which
modern macOS requires for runnable executables.

- built with `make freestanding` on local macOS/aarch64, or explicitly with
  `make freestanding-macos`
- writes binaries to `build/freestanding-macos-aarch64/`
- currently builds 73 tools: `true`, `false`, `echo`, `printf`, `basename`,
  `dirname`, `yes`, `rev`, `seq`, `expr`, `test`, `nl`, `tac`, `expand`,
  `unexpand`, `fold`, `wc`, `head`, `cat`, `cut`, `tr`, `uniq`, `cmp`, `comm`,
  `join`, `paste`, `printenv`, `pwd`, `mkdir`, `rmdir`, `tee`, `which`,
  `readlink`, `realpath`, `sleep`, `file`, `strings`, `hexdump`, `od`,
  `md5sum`, `sha256sum`, `sha512sum`, `dd`, `touch`, `truncate`, `sync`, `bc`,
  `split`, `shuf`, `fmt`, `column`, `tsort`, `mktemp`, `clear`, `date`,
  `uname`, `hostname`, `whoami`, `id`, `groups`, `ls`, `du`, `stat`, `df`,
  `rm`, `cp`, `mv`, `ln`, `chmod`, `chown`, `chgrp`, `free`, and `kill`
- uses `src/platform/macos/` plus `src/arch/aarch64/macos/` for Darwin-specific
  behavior
- compiles with freestanding-oriented flags and `-nodefaultlibs -lSystem`, so
  the project code does not call the C standard library even though the binary
  has the unavoidable macOS system ABI dependency
- is intentionally smaller than the Linux freestanding target until process,
  terminal, networking, archive/compression, richer reporting, and larger
  application-level tool dependencies are implemented

## SELF-HOSTED BUILD

The self-hosted build reuses the in-tree `ncc` compiler for the hosted tool set.

- built with `make selfhost`
- writes binaries to `build/selfhost-<os>-<arch>/`
- uses the hosted `ncc` binary as `CC` while still relying on the system linker
- is the main bootstrap-progress check for Linux/x86-64 today

This path matters especially for shared runtime changes, shell support code,
and anything that adds new low-level dependencies.

## TARGETS

    make               — on macOS build the local hosted set; on Linux build host plus freestanding
    make host          — build the hosted POSIX binaries under build/host-<os>-<arch>/ with compatibility symlinks in build/
    make freestanding  — on Linux build the static syscall-only target under build/freestanding-linux-$(TARGET_ARCH)/; on local macOS/aarch64 build the freestanding-ish Darwin subset
    make freestanding-macos — build the early native macOS arm64 freestanding-ish subset under build/freestanding-macos-aarch64/
    make freestanding-windows — build the native no-libc Windows PE subset under build/freestanding-windows-$(WINDOWS_TARGET_ARCH)/
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
Windows userland target. Install the MSYS GCC package for hosted POSIX builds
and the UCRT64 Clang/lld packages for freestanding Linux output:

  pacman -Syuu
  pacman -S --needed base-devel gcc mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld

Then start the MSYS shell for the hosted build, change to the repository, and
use Clang for the Linux freestanding path:

  cd /c/Users/Mathias\ Schindler/newos
  make host CC=gcc
  make freestanding TARGET_ARCH=x86_64 TARGET_CC=clang
  make freestanding-windows WINDOWS_TARGET_CC=clang

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

`make freestanding` remains the main target: it builds Linux ABI binaries using
the raw syscall backend under `src/platform/linux/` and `src/arch/*/linux/`.
`make freestanding-windows` is the early native Windows path. It links PE
executables directly against Kernel32, Ws2_32, and Bcrypt where needed. The
Windows subset now covers the small text/core tools, comparison/checksum/image,
path/filesystem, regex/archive/awk/XML groups, plus `wtf`, `editor`, `mail`, and
the `ncc` compiler executable. The backend has startup, argument parsing,
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
- Windows native hosted binaries are not supported yet; use MSYS2 for hosted POSIX builds and `make freestanding-windows` for the native no-CRT PE backend
- There is no install or staging-prefix workflow yet
- Hosted success and freestanding success should be treated as related but separate checks

## SEE ALSO

man, project-layout, compiler, platform, macos, testing
