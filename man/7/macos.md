# MACOS

## NAME

macos - macOS/AArch64 build model, limitations, and design choices

## DESCRIPTION

macOS is an active contributor and day-to-day development platform for the
repository. On this platform, the project focuses on a hosted AArch64 build for
local use while still keeping the shared code structured so that the Linux
freestanding path remains viable.

This page documents the current macOS-specific expectations so that the build
behavior is explicit rather than surprising.

## CURRENT MODEL

On a Mac, the normal build outputs are native Mach-O executables for the local
machine.

- `make` builds the local hosted tool set by default
- `make host` builds the same hosted local binaries under
  `build/host-macos-aarch64/` with compatibility symlinks in `build/`
- `make freestanding` currently resolves to the local hosted build on macOS by
  default rather than producing a separate libc-free target tree

This policy exists because the repository is actively developed on macOS and
contributors usually want runnable local binaries first.

## LIMITATIONS

The project does not currently treat macOS as a true freestanding userland
execution target in the Linux sense.

- normal macOS executables are expected to follow Mach-O conventions and use
  the system runtime
- fully static, libc-free user executables are not the primary supported model
  on modern macOS/AArch64
- the raw-syscall freestanding environment in this repository is therefore a
  Linux target, not a Darwin one

When the manuals say "freestanding" without further qualification, they should
usually be read as referring to the Linux syscall-only build.

## TECHNICAL DECISIONS

The current macOS strategy is intentional.

- keep most logic in `src/shared/` so it can be reused by both hosted and
  freestanding builds
- route OS interaction through `src/shared/platform.h` so hosted macOS work does
  not force libc details into every tool
- treat the hosted macOS build as the fast iteration loop for tools, shell
  behavior, compiler work, and documentation updates
- continue using the Linux freestanding build as the portability and minimal
  runtime check for syscall-only operation

In short: macOS is the main developer workstation environment, while Linux is
still the reference freestanding runtime target.

## OPTIONS AND OVERRIDES

If you need a different target than the local hosted build, use explicit build
variables or direct compiler invocations rather than assuming the default `make`
path will cross-compile for you.

Examples include selecting a Linux freestanding architecture with `TARGET_ARCH`
on a Linux host or invoking `ncc` with an explicit backend target.

## SEE ALSO

man, build, platform, compiler, project-layout, userland
