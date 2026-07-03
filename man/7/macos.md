# MACOS

## NAME

macos - macOS/AArch64 build model, limitations, and design choices

## DESCRIPTION

macOS is an active contributor and day-to-day development platform for the
repository. On this platform, the preferred build and validation path is
`make freestanding`, which produces project-linked Mach-O arm64 tools with no
intended dylib imports. The hosted AArch64 build remains available as a
secondary POSIX verification and bring-up path, but it is no longer the design
center for normal macOS work.

This page documents the current macOS-specific expectations so that the build
behavior is explicit rather than surprising.

## CURRENT MODEL

On a Mac, the normal build outputs are native Mach-O executables for the local
machine.

- `make freestanding` on local macOS/aarch64 is the preferred path; it builds
  the project-linked Darwin tool set under `build/macos-aarch64/`
- `make host` builds hosted POSIX binaries under `build/host-macos-aarch64/`
  with compatibility symlinks in `build/`; use it for secondary verification,
  quick POSIX bring-up, and host-side comparisons
- `make` builds the local hosted tool set by default for compatibility with the
  historical local loop, but `make freestanding` is the target that should be
  used when validating the no-libSystem macOS build
- `make freestanding-macos` builds the older Apple-ld/`libSystem` comparison
  target under `build/freestanding-macos-aarch64/`
- the current project-linked target builds the declared macOS tool surface,
  spanning small core commands, text and file filters, path metadata, symlink
  queries, checksums, math, identity, directory listing, filesystem mutation,
  process spawning/listing, terminal mode, pagers, archive/compression, image
  metadata, object inspection, XML, SQL, manuals, DNS/network tools, TLS-backed
  clients, PGP, PDF, `ncc`, `ssh`, `sshd`, `httpd`, `sh`, `editor`, `mail`,
  `make`, `service`, and the other tools declared for the macOS project-linked
  build in the root `Makefile`
- host-mutating admin paths remain intentionally conservative when the Darwin
  implementation is not validated; those commands build and expose usage/read
  paths, but may report unsupported operations rather than changing the host

This policy exists because the repository is actively developed on macOS, and
the project-linked Darwin path is now strong enough to act as the normal local
freestanding build. It exercises the project runtime, the Darwin platform
boundary, direct syscall wrappers, and the project linker while producing real
launchable binaries that avoid `libSystem` imports.

## LIMITATIONS

The project does not treat macOS as a true freestanding userland execution
target in the Linux sense.

- normal Apple-distributed macOS executables are expected to follow Mach-O
  conventions and use the system runtime
- the project-linked Darwin target deliberately avoids libc calls and dylib
  imports, including `libSystem`; this is the preferred project validation path
  on local macOS, but it is not an Apple-recommended distribution model
- the executable still has to satisfy Darwin and Mach-O loader rules, including
  valid segment layout, entry/load commands, dyld metadata where needed, and an
  ad-hoc CodeDirectory signature
- `make freestanding-macos` remains the conventional Apple-ld/`libSystem`
  comparison target when that model is needed for size or behavior checks

When the manuals say "syscall-only freestanding" without further qualification,
they should usually be read as referring to the Linux build. The macOS path is
better described as project-linked, no-libSystem Mach-O.

## TECHNICAL DECISIONS

The current macOS strategy is intentional.

- keep most logic in `src/shared/` so it can be reused by both hosted and
  freestanding builds
- route OS interaction through `src/shared/platform.h` so hosted macOS work does
  not force libc details into every tool
- treat `make freestanding` as the primary macOS build when checking tools,
  shared runtime, platform wrappers, linker behavior, and syscall-level changes
- treat the hosted macOS build as a secondary fast POSIX loop for quick tool
  bring-up, host-side comparisons, and debugging before platform support exists
- continue using the Linux freestanding build as the portability and minimal
  runtime check for syscall-only operation
- keep growing the Darwin surface by adding platform primitives under
  `src/platform/macos/` and `src/arch/aarch64/macos/`, rather than adding
  platform branches to shared code or tools
- treat successful execution on the target OS/architecture as the primary
  validity metric for deliberately compact binaries; host inspection tools may
  disagree with or under-report these files, especially when ELF section tables
  or optional Mach-O payloads have been removed

In short: macOS is the main developer workstation environment and has a
preferred project-linked no-libSystem `make freestanding` build; Linux is still
the reference raw-syscall freestanding runtime target.

## OPTIONS AND OVERRIDES

If you need a different target than the local macOS project-linked build, use
explicit build variables or direct compiler invocations rather than assuming the
default `make` path will cross-compile for you.

Examples include selecting a Linux freestanding architecture with `TARGET_ARCH`
on a Linux host, invoking `ncc` with an explicit backend target, running
`make host` for the hosted POSIX comparison path, or running
`make freestanding-macos` for the older Apple-ld/`libSystem` comparison build.

## SEE ALSO

man, build, platform, compiler, project-layout, userland
