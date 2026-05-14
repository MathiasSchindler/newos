# newos

newos is an experimental userland project for a Linux-ABI-compatible operating system.

In broad terms, this repository is a growing collection of command-line tools, shell support code, a self-hosting C compiler, shared runtime code, platform backends, and in-tree documentation designed to build on host systems such as macOS and Linux while also targeting a freestanding Linux environment.

The project has been written with the help of a finetuned version of GPT 5.4, with an emphasis on portability, small utilities, clear separation between tool logic and platform-specific code, and a freestanding-first design.

## Scope

The repository currently focuses on:

- a broad and growing Unix-style userland of command-line programs
- shell support and shared support code for strings, I/O, archives, hashing, crypto, regex, Unicode text handling, and common tool behavior
- the self-hosting C compiler ncc and its supporting infrastructure
- platform layers for hosted POSIX builds, Linux freestanding targets, and self-hosted rebuilds through ncc where supported
- a small in-tree manual system, with pages stored under [man](man) and viewable through the project's own man tool

## Current status

The host-side build and test workflow are active and in regular use on macOS and Linux. Windows is a new contributor environment for the project; the current practical route is MSYS2 while native Windows platform support is developed.

The userland has expanded substantially across filesystem, text, process, network, archive, build, math, and system-reporting tools. The hosted Linux workflow is active alongside macOS, Linux freestanding builds exercise the libc-free syscall path where available, and the self-hosted build path through ncc is now a regular bootstrap-progress check.

The repository also includes a lightweight manual browser and a growing set of manual pages for tools, subsystems, and design notes.

## Testing

The repository includes a structured shell-based test suite under [tests](tests).

- the main entry point is `make test`
- Phase 1 per-tool checks live under [tests/phase1](tests/phase1)
- higher-level smoke suites are run by [tests/run_smoke_tests.sh](tests/run_smoke_tests.sh)
- shared helpers live in [tests/lib](tests/lib)
- grouped suites live in [tests/suites](tests/suites)

On Linux, `make test` also exercises representative freestanding binaries. On macOS, freestanding Linux tests are skipped by default and the local hosted workflow remains the normal path.

On Windows, install MSYS2 with the MSYS GCC package for the hosted POSIX build
and the UCRT64 Clang/lld toolchain for freestanding output. The first useful
checks are:

```
make host CC=gcc
make freestanding TARGET_ARCH=x86_64 TARGET_CC=clang
make freestanding-windows WINDOWS_TARGET_CC=clang
```

`make freestanding` still emits Linux ABI binaries through the raw Linux
syscall backend. `make freestanding-windows` is the initial native PE path; it
currently builds a small no-libc Windows subset (`true`, `false`, `echo`,
`printf`, `dirname`, `basename`, `cat`, `head`, `tail`, `nl`, `rev`, `fold`,
`uniq`, `wc`, `cut`, `tr`, `expand`, `unexpand`, `pwd`, `hostname`, and
`uname`), plus comparison, checksum, image, path, and basic filesystem tools
such as `cmp`, `comm`, `join`, `paste`, `tac`, `sleep`, `file`, `readlink`,
`realpath`, `strings`, `hexdump`, `od`, `md5sum`, `sha256sum`, `sha512sum`,
`test`, `which`, `printenv`, `tee`, `mkdir`, `rmdir`, `truncate`, `sync`,
`imgmeta`, `imginfo`, `imgcheck`, `bc`, `expr`, and `seq`. `wtf` builds and can
fetch summaries through the native Winsock/TLS path; certificate validation is
not wired to the Windows trust store yet, so treat it as an early bring-up check
rather than a hardened HTTPS client.
To launch hosted MSYS tools directly from PowerShell or `cmd.exe`, keep the
MSYS runtime on `PATH`, for example `C:\msys64\usr\bin`. Copying
`msys-2.0.dll` beside the tools is not supported because it relocates the MSYS
root away from the installed environment.
In PowerShell, dot-source the helper once per terminal session:

```
. .\activate-host-msys.ps1
```

If local PowerShell execution policy blocks scripts, use the command runner:

```
.\run-host-msys.cmd .\build\host-msys-posix-x86_64\ls.exe
```

## Userland shell

On Linux, [run-userland.sh](run-userland.sh) starts an isolated terminal session backed by the freestanding build. It runs `make freestanding` by default, then execs the project `env` with an empty environment, `PATH` pointing only at `build/freestanding-linux-$(uname -m)`, `MANPATH` pointing at the repository manuals, and the project `sh` as the shell:

```
./run-userland.sh
./run-userland.sh --no-build
./run-userland.sh -- 'man ls'
```

This does not install files or change the host system; closing the shell returns to the normal Ubuntu environment.

For a smoke test from inside that environment, run:

```
./test.sh
./test.sh --no-build
```

The test enters the freestanding shell with the isolated `PATH` and checks core command lookup, text tools, filesystem operations, archive tools, shell behavior, manuals, SQL, and a bounded local `httpd`/`wget` round trip.

## Documentation

Manual pages live under [man](man), with user-facing commands typically documented in [man/1](man/1) and broader design notes in [man/7](man/7).

The repository also includes its own man program at [src/tools/man.c](src/tools/man.c), so the same tree can provide and browse its own documentation after a build.

Useful design pages include [man/7/userland.md](man/7/userland.md), [man/7/build.md](man/7/build.md), [man/7/testing.md](man/7/testing.md), [man/7/runtime.md](man/7/runtime.md), and [man/7/unicode.md](man/7/unicode.md).

## Benchmarks

A separate benchmark area now lives under [tests/benchmarks](tests/benchmarks) for host-side performance comparisons against the standard system tools.

You can run it with make benchmark.
