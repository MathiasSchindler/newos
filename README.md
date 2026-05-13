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

The host-side build and test workflow are active and in regular use on macOS and Linux.

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
