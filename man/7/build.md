# BUILD

## NAME

build - hosted and freestanding build workflow

## DESCRIPTION

The repository uses one root `Makefile` for both normal development and the
freestanding target. The same tools are compiled in both modes where practical,
but they are linked against different backends and start-up paths.

For most contributors, the usual loop is `make host` followed by `make test`.
On Linux, `make freestanding` is the cross-compilation path for the libc-free
Linux target and is mainly used to keep the syscall-only environment healthy and
to catch regressions that do not show up in the hosted build. On macOS, the
local default remains the hosted build unless an explicit alternative target is
requested.

## HOSTED BUILD

The hosted build is the normal development path.

- built with `make host`
- writes binaries to `build/`
- uses the POSIX backend in `src/platform/posix/`
- is the variant exercised by the smoke tests and benchmarks
- is the fastest loop for day-to-day tool, shell, and compiler work

When you are changing visible behavior or adding a new utility, this is usually
the first mode to get green.

## FREESTANDING BUILD

The freestanding build is the Linux target without libc.

- built with `make freestanding`
- writes binaries to `build/linux-$(TARGET_ARCH)/`
- uses `src/platform/linux/` plus `src/arch/$(TARGET_ARCH)/linux/`
- links with the minimal `crt0.S` entry path and direct syscalls
- is where ABI, start-up, and portability mistakes become visible

This path matters especially for shared runtime changes, shell support code,
and anything that adds new low-level dependencies.

## TARGETS

    make               — on macOS build the local hosted set; on Linux build host plus freestanding
    make host          — build the hosted POSIX binaries under build/
    make freestanding  — on Linux build the static syscall-only target under build/linux-$(TARGET_ARCH)/; on macOS default to the local hosted build
    make test          — build host binaries and run tests/run_smoke_tests.sh
    make benchmark     — build host binaries and run tests/benchmarks/run_benchmarks.sh
    make clean         — remove build output

## TARGET SELECTION

`TARGET_ARCH` selects the freestanding arch layer (`x86_64` or `aarch64`). The
default follows the host on Linux and otherwise falls back to `aarch64`.
`TARGET_TRIPLE` is derived from that choice unless it is overridden manually.

Typical examples:

    make freestanding TARGET_ARCH=x86_64
    make freestanding TARGET_ARCH=aarch64

## COMMON WORKFLOW

A common contributor sequence is:

    make host
    make test
    make freestanding

Use the hosted build for quick iteration, then rerun the freestanding path when
a change touches runtime code, platform code, startup code, or the compiler.

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
- Hosted and freestanding outputs are intentionally separate. A passing hosted
  build does not guarantee the syscall-only target is also healthy.
- If a new shared runtime helper is added, make sure any explicit special-case
  build rules and compiler-driver source lists stay in sync.
- Header dependency tracking is lightweight. If shared headers or build flags
  change, prefer `make clean && make host`.

## LIMITATIONS

- The Linux freestanding build currently assumes Clang plus `lld`
- On macOS, the default build behavior favors local runnable binaries over a
  separate fully freestanding Darwin userland target
- `make test` exercises the hosted binaries only
- There is no install or staging-prefix workflow yet
- Hosted success and freestanding success should be treated as related but
  separate checks

## SEE ALSO

man, project-layout, compiler, platform, macos, testing
