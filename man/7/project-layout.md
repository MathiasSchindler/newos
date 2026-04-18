# PROJECT-LAYOUT

## NAME

project-layout - overview of the repository structure and current layering

## DESCRIPTION

The repository is organized around a broad Unix-style userland, a self-hosting C compiler, shared runtime code, and platform-specific implementations. All components are designed to build on host systems such as macOS and to cross-compile to a freestanding Linux/AArch64 target.

## STRUCTURE

- src/tools contains the command-line tool implementations
- src/shared contains runtime helpers, shell subsystem code, and shared utilities
- src/compiler contains the compiler frontend, IR, and code generation backend
- src/platform/posix contains the hosted POSIX platform layer
- src/platform/linux contains the freestanding Linux platform layer
- src/arch/aarch64/linux contains the AArch64 CRT and syscall definitions
- build contains compiled host binaries
- build/linux-aarch64 contains freestanding cross-compiled binaries
- tests contains smoke-test suites, shared helpers, and benchmarks
- man contains repository-local manual pages

## INTENT

The long-term design aims at dependency-free, statically linked binaries and a freestanding environment while keeping hosted development practical for day-to-day work.

## LIMITATIONS

- Only Linux/AArch64 is supported as a freestanding target at present
- The hosted build targets the POSIX layer only; Windows is not supported
- Not all tools have repository-local manual pages yet

## SEE ALSO

man, shell, compiler, runtime, platform, testing, build
