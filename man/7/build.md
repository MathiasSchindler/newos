# BUILD

## NAME

build - the freestanding and hosted build workflow

## DESCRIPTION

The project uses a single GNU Makefile at the repository root to drive both a hosted development build and a freestanding cross-compiled build targeting Linux/AArch64. Parallelism is enabled automatically when neither -j nor --jobserver flags are already present in MAKEFLAGS.

## TARGETS

    make            — build both host and freestanding targets in parallel
    make host       — compile all tools against the POSIX platform layer using the host CC
    make freestanding — cross-compile all tools against the Linux platform layer using TARGET_CC
    make test       — build host targets then run tests/run_smoke_tests.sh
    make benchmark  — build host targets then run tests/benchmarks/run_benchmarks.sh
    make clean      — remove the build directory

## CONFIGURATION VARIABLES

- CC — C compiler used for the hosted build (default: cc)
- TARGET_CC — compiler used for cross-compilation; auto-detected from clang in PATH or Homebrew (default: clang)
- CFLAGS — flags applied to both builds; includes -std=c11 -Wall -Wextra -Wpedantic -O2 and the required -I paths
- FREESTANDING_CFLAGS — additional flags for the freestanding build: -ffreestanding -fno-builtin -fno-stack-protector and related options
- BUILD_DIR — output directory for host binaries (default: build)
- TARGET_BUILD_DIR — output directory for freestanding binaries (default: build/linux-aarch64)
- TARGET_TRIPLE — Clang target triple for cross-compilation (default: aarch64-linux-none)
- PARALLEL_JOBS — number of parallel jobs (default: number of logical CPUs)

## FREESTANDING BUILD DETAILS

The freestanding build passes -nostdlib -static -fuse-ld=lld to the compiler and links the Linux platform layer, the AArch64 CRT (src/arch/aarch64/linux/crt0.S), and all shared runtime sources in place of libc. The resulting binaries are statically linked ELF executables that require only the Linux kernel ABI.

The shell (sh) and compiler (ncc) have dedicated Makefile rules that explicitly list all their source dependencies; all other tools share a generic pattern rule.

## LIMITATIONS

- The freestanding build requires a Clang installation capable of targeting aarch64-linux-none and lld as linker; GCC cross-toolchains are not currently supported
- Incremental builds track direct source dependencies; changes to transitively included headers may require a clean rebuild
- The build system does not yet support installing binaries to a staging root or system prefix

## SEE ALSO

man, project-layout, compiler, platform, testing
