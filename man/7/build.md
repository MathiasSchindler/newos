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
- defaults to static PIE output with stack protector instrumentation
- is where ABI, start-up, and portability mistakes become visible

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
    make freestanding  — on Linux build the static syscall-only target under build/freestanding-linux-$(TARGET_ARCH)/; on macOS default to the local hosted build
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
  build rules and compiler-driver source lists stay in sync.
- Header dependency tracking is lightweight. If shared headers or build flags
  change, prefer `make clean && make host`.

## LIMITATIONS

- The Linux freestanding build currently assumes Clang plus `lld`
- On macOS, the default build behavior favors local runnable binaries over a separate fully freestanding Darwin userland target
- There is no install or staging-prefix workflow yet
- Hosted success and freestanding success should be treated as related but separate checks

## SEE ALSO

man, project-layout, compiler, platform, macos, testing
