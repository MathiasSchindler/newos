# PROJECT-LAYOUT

## NAME

project-layout - overview of the repository structure and layering rules

## DESCRIPTION

The tree is organized by responsibility: user-facing tools, reusable shared
code, compiler internals, and platform-specific code are kept separate so that
hosted and freestanding builds can share as much logic as possible.

## STRUCTURE

- `src/tools/` — command entry points and tool-specific logic; the default home
  for a new utility
- `src/shared/` — reusable runtime helpers, shared utilities, and the shell
  subsystem
- `src/compiler/` — the `ncc` frontend, IR, backends, and object writers
- `src/platform/posix/` — hosted development and test implementation
- `src/platform/linux/` — freestanding raw-syscall implementation
- `src/arch/aarch64/linux/` — startup and syscall ABI glue for the freestanding
  AArch64 target
- `src/arch/x86_64/linux/` — startup and syscall ABI glue for the freestanding
  x86-64 target
- `tests/` — smoke suites, helpers, benchmarks, and run-time logs under
  `tests/tmp/`
- `man/` — repository-local manuals; `man/1` and `man/7` are the active home for
  current documentation and contributor-facing project notes
- `build/` — generated binaries and other build output

## LAYERING RULES

- Most tools should depend on `src/shared/` and `platform.h`, not on direct OS
  headers or compiler internals.
- Shell-specific code stays in `src/shared/shell_*` and is compiled only into
  `sh`.
- `src/compiler/` is largely private to `ncc`; other tools should not grow hard
  dependencies on it.
- OS-specific behavior belongs in `src/platform/*` or `src/arch/*`, not in the
  generic tool code.
- Legacy planning notes may still exist elsewhere in the tree, but the manual
  pages under `man/` should be treated as the current source of truth.

## LIMITATIONS

- The freestanding path currently targets Linux/AArch64 and Linux/x86-64
- Hosted development assumes a POSIX-like system; Windows is not a supported
  contributor platform
- Manual pages are kept in-tree and may lag very recent behavior changes until
  they are refreshed alongside the code

## SEE ALSO

man, shell, compiler, runtime, platform, testing, build, userland
