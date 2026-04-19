# BUILD

## NAME

build - hosted and freestanding build workflow

## DESCRIPTION

The repository uses one root `Makefile` for both normal development and the
freestanding target. For most contributors, the usual loop is `make host`
followed by `make test`. `make freestanding` is the cross-compilation path for
the libc-free Linux target and is mainly used to keep the freestanding path
healthy.

## TARGETS

    make               — build both host and freestanding targets
    make host          — build the hosted POSIX binaries under build/
    make freestanding  — cross-compile static Linux binaries under build/linux-$(TARGET_ARCH)/
    make test          — build host binaries and run tests/run_smoke_tests.sh
    make benchmark     — build host binaries and run tests/benchmarks/run_benchmarks.sh
    make clean         — remove build output

## CONTRIBUTOR NOTES

- Most new tools only need `src/tools/name.c` plus an entry in `TOOLS` in the
  `Makefile`; the generic pattern rules handle both build variants.
- `sh` and `ncc` have explicit rules because they pull in additional shared
  subsystems. A new tool with special dependencies should follow that pattern.
- Hosted and freestanding outputs are intentionally separate. A passing hosted
  build does not guarantee the syscall-only target is also healthy.
- `TARGET_ARCH` selects the freestanding arch layer (`x86_64` or `aarch64`).
  The default follows the host on Linux and otherwise falls back to `aarch64`.
- Header dependency tracking is lightweight. If shared headers or build flags
  change, prefer `make clean && make host`.

## LIMITATIONS

- The freestanding build currently assumes Clang plus `lld`
- `make test` exercises the hosted binaries only
- There is no install or staging-prefix workflow yet

## SEE ALSO

man, project-layout, compiler, platform, testing
