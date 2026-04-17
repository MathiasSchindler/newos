# newos

newos is an experimental userland project for a Linux-ABI-compatible operating system.

In broad terms, this repository is a growing collection of command-line tools, shell support code, a self-hosting C compiler, shared runtime code, and platform backends that are designed to build on host systems such as macOS while also targeting a freestanding Linux environment.

The project has been written with the help of a finetuned version of GPT 5.4, with an emphasis on portability, small utilities, clear separation between tool logic and platform-specific code, and a freestanding-first design.

## Scope

The repository currently focuses on:

- a broad and growing Unix-style userland of command-line programs
- shell support and shared support code for strings, I/O, archives, and hashing
- the self-hosting C compiler ncc and its supporting infrastructure
- platform layers for hosted POSIX builds and freestanding Linux/AArch64 builds

## Current status

The host-side build and smoke-test workflow are active and in regular use on macOS.

The userland has expanded substantially, and the compiler is already capable of handling a large portion of the repository, though the newest additions still expose some remaining self-hosting gaps that are being closed incrementally.

## Testing

The repository now includes a structured smoke-test suite under [tests](tests).

- the entry point is [tests/run_smoke_tests.sh](tests/run_smoke_tests.sh)
- shared helpers live in [tests/lib](tests/lib)
- grouped suites live in [tests/suites](tests/suites)

You can run the suite with make test.

## Benchmarks

A separate benchmark area now lives under [tests/benchmarks](tests/benchmarks) for host-side performance comparisons against the standard system tools.

You can run it with make benchmark.

## License

This project is released under CC-0.
