# MACOS

## NAME

macos - macOS/AArch64 build model, limitations, and design choices

## DESCRIPTION

macOS is an active contributor and day-to-day development platform for the
repository. On this platform, the project keeps a hosted AArch64 build for fast
local iteration, but the normal freestanding target is now a project-linked
Mach-O arm64 build with no intended dylib imports.

This page documents the current macOS-specific expectations so that the build
behavior is explicit rather than surprising.

## CURRENT MODEL

On a Mac, the normal build outputs are native Mach-O executables for the local
machine.

- `make` builds the local hosted tool set by default
- `make host` builds the same hosted local binaries under
  `build/host-macos-aarch64/` with compatibility symlinks in `build/`
- `make freestanding` on local macOS/aarch64 builds the project-linked Darwin
  tool set under `build/newlinker-macos-aarch64/`
- `make macos-newlinker-tools` builds that same project-linked target explicitly
- `make freestanding-macos` builds the older Apple-ld/`libSystem` comparison
  target under `build/freestanding-macos-aarch64/`
- the current subset contains the full 194-tool set spanning small core commands,
  text and file filters, path metadata, symlink queries, checksums, `bc`,
  identity, directory listing, filesystem mutation, process spawning/listing,
  terminal mode, pagers, archive/compression, image metadata, object
  inspection, XML, `sql`, `man`, `pstree`, `wget`, `ncc`, `netcat`, DNS
  lookup/query tools, `ssh`, `sshd`, `httpd`, `ping`, `ping6`, read-only `ip`
  link/address inspection, DHCP probing, `dmesg`, `mknod`, `mount`, `umount`,
  `shutdown`, `service`, `sh`, `editor`, `mail`, `make`, basic TCP/TLS client
  networking for `wtf`, `free`, `kill`, sleep, touch, truncate, sync, and basic
  `dd`
- host-mutating admin paths remain intentionally conservative when the Darwin
  implementation is not validated; those commands build and expose usage/read
  paths, but may report unsupported operations rather than changing the host

This policy exists because the repository is actively developed on macOS and
contributors usually want runnable local binaries first, while still having a
native Darwin path that exercises the freestanding platform boundary, project
runtime, and in-tree Mach-O linker.

## LIMITATIONS

The project does not treat macOS as a true freestanding userland execution
target in the Linux sense.

- normal macOS executables are expected to follow Mach-O conventions and use the system runtime
- the normal project-linked Darwin target deliberately avoids libc calls and
  dylib imports, including `libSystem`; this is useful for the project but is
  not an Apple-recommended distribution model
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
- treat the hosted macOS build as the fast iteration loop for tools, shell
  behavior, compiler work, and documentation updates
- continue using the Linux freestanding build as the portability and minimal
  runtime check for syscall-only operation
- keep growing the Darwin subset by adding platform primitives under
  `src/platform/macos/` and `src/arch/aarch64/macos/`, rather than adding
  platform branches to shared code or tools
- treat successful execution on the target OS/architecture as the primary
  validity metric for deliberately compact binaries; host inspection tools may
  disagree with or under-report these files, especially when ELF section tables
  or optional Mach-O payloads have been removed

In short: macOS is the main developer workstation environment and has a normal
project-linked no-libSystem build; Linux is still the reference raw-syscall
freestanding runtime target.

## OPTIONS AND OVERRIDES

If you need a different target than the local hosted build, use explicit build
variables or direct compiler invocations rather than assuming the default `make`
path will cross-compile for you.

Examples include selecting a Linux freestanding architecture with `TARGET_ARCH`
on a Linux host or invoking `ncc` with an explicit backend target.

## SEE ALSO

man, build, platform, compiler, project-layout, userland
