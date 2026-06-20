# PLATFORM

## NAME

platform - the OS abstraction layers for hosted and freestanding builds

## DESCRIPTION

The platform component keeps shared code away from direct OS calls. Runtime and
tool code are expected to program against `src/shared/platform.h`, which is then
backed by the hosted POSIX implementation, the freestanding raw-Linux syscall
implementation, or the native Windows freestanding PE implementation.
On local macOS/aarch64 there is also a project-linked Darwin path: objects are
Mach-O, final executables are written by the in-tree linker, and the platform
layer uses Darwin syscalls rather than libc calls from the tools.

Windows is being treated as a new contributor workstation environment first.
Hosted Windows builds still run inside MSYS2 and use the POSIX backend, while
`tests/windows/build-windows-freestanding.ps1` uses `src/platform/windows/` to build native
PE executables without the MSYS POSIX runtime or the Microsoft C runtime.

## BUILD-MODE SUMMARY

The platform abstraction exists so that most tool code can stay the same across
the normal freestanding builds and the secondary hosted POSIX build.

In practice:

- `make freestanding` is the normal macOS and Linux path
- the hosted build is for secondary POSIX verification, testing, and early
  platform bring-up when native code is not ready yet
- the Linux freestanding build is the raw Linux ABI and minimal startup path
- the macOS freestanding build is the project-linked native Darwin path; the
  intended outputs have no dylib imports and no C standard library dependency,
  even though this is outside Apple's recommended executable model
- `make freestanding-macos` is kept as the older Apple-ld/`libSystem`
  comparison path
- a bug that appears only in one mode usually means the abstraction boundary is
  missing an implementation detail or has leaked an assumption from the other
  side

## FREESTANDING-FIRST POLICY

The platform layer exists to keep the project from quietly drifting into a
host-libc-only design.

- Prefer shared logic plus narrow `platform_*` primitives rather than direct
  POSIX calls in every tool.
- the hostedness escape hatch should stay narrow and should not become the
  default implementation strategy.
- If a capability matters in both build modes, implement it in both backends or
  document clearly why it is hosted-only.

## STRUCTURE

### Hosted POSIX layer (`src/platform/posix`)

Used by `make host` and by the smoke tests. This is the hosted POSIX comparison
path, not the design center for ordinary macOS or Linux builds.

- `fs.c` — file, directory, and path operations
- `process.c` — spawn, wait, signal, terminal, and environment handling
- `identity.c` — users, groups, sessions, and related lookups
- `net.c` — sockets plus helpers for tools such as `ping` and `netcat`
- `time.c` — clocks, formatting, uptime, and sleep helpers
- `tls.c` — platform-facing TLS client glue backed by the shared TLS code

### Freestanding Linux layer (`src/platform/linux`)

Used by `make freestanding` on Linux. This layer talks to the Linux kernel ABI
directly and does not depend on libc.

- `fs.c`, `process.c`, `identity.c`, `net.c`, `time.c`, and `tls.c` mirror the
  hosted interface using syscalls and freestanding shared code
- `thread.c` — project-owned Linux worker-thread and wait/wake substrate backed by `clone` and `futex`; see [threading](threading.md)
- `stack_guard.c` — stack-canary guard initialization and failure handling for
  freestanding binaries when compiler instrumentation is enabled
- `profiler_runtime.c` — GCC/Clang `-finstrument-functions` hooks for Linux
  profiling builds, using libc when hosted and raw syscalls when freestanding

### Project-linked macOS layer (`src/platform/macos`)

Used by `make freestanding` on local macOS/aarch64 for the normal
project-linked Mach-O build, and by `make freestanding-macos` for the older
Apple-ld/`libSystem` comparison build. The normal path is the native Darwin
arm64 approximation of the freestanding idea: shared/tool code remains
universal, the platform boundary owns the OS details, and the final executable
is intended to import no dylibs. This works for the project tools but is not the
model Apple recommends for general macOS software.

- `freestanding.c` — Darwin-backed read, write, close, open, seek,
  environment, metadata, symlink, sleep, checksum/file helper, basic filesystem
  mutation, process id, and page allocation primitives for the declared macOS
  project-linked tool surface
- `profiler_runtime.c` — GCC/Clang `-finstrument-functions` hooks for macOS
  project-linked profiling builds; trace output uses raw Darwin file syscalls,
  while timestamps use the arm64 virtual counter to keep the hook hot path small
- `src/arch/aarch64/macos/syscall.h` — inline Darwin syscall helpers for the
  platform adapter

The normal project-linked path builds the declared macOS tool surface under
`build/newlinker-macos-aarch64/`. The tools use the same project runtime and
platform interface as other freestanding targets, with Darwin-specific syscall,
startup, and Mach-O loader details hidden behind `src/platform/macos/`,
`src/arch/aarch64/macos/`, and the in-tree linker. Some privileged or
host-mutating operations remain conservative on Darwin when the implementation
has not been validated; those commands may expose usage/read paths while
reporting unsupported operations instead of changing the host.

### Architecture glue (`src/arch/*/linux`)

- `crt0.S` — minimal process startup for the freestanding binaries
- `syscall.h` — inline syscall helpers for the selected Linux target ABI
- `syscall_stubs.S` — x86-64 out-of-line syscall entry helpers used by the freestanding platform layer

### Native Windows freestanding layer (`src/platform/windows`)

Used by `tests/windows/build-windows-freestanding.ps1`. This layer links PE executables
directly against the minimal Windows system DLL imports needed by the tools and
keeps Win32 details out of shared runtime and tool code.

- `core.c` — PE startup, argument parsing, fd/handle mapping, console I/O,
  file/path helpers, environment lookup, time, terminal mode support, Winsock,
  and process exit
- `tls.c` — platform-facing TLS client glue backed by the shared TLS code and
  Windows random byte generation

The still-growing parts are process spawning, directory enumeration, symlink
compatibility, fuller identity/session reporting, and certificate validation
against the Windows trust store.

## CONTRIBUTOR BOUNDARIES

- Shared code should call `platform_*` helpers rather than `open(2)`,
  `read(2)`, `getenv(3)`, or other OS APIs directly.
- If a new capability is needed in both build modes, add it to the platform
  interface and implement both backends.
- ABI- or startup-specific code belongs under `src/arch/*`, not in the generic
  runtime or tool sources.

## LIMITATIONS

- Hosted development assumes a POSIX environment; the Linux freestanding target currently focuses on AArch64 and x86-64
- On macOS/aarch64, `make freestanding` routes to the project-linked Mach-O path with no intended dylib imports; `make freestanding-macos` remains available as the older Apple-ld/`libSystem` comparison path, and `make host` remains the hosted POSIX path
- Compact ELF and Mach-O outputs are judged by whether they execute correctly on the target platform with reasonable resource use. Some system inspection tools may handle these deliberately small files poorly; prefer the project `file`, `readelf`, `objdump`, `nm`, `size`, and `imgcheck` tools for diagnostics.
- Native Windows freestanding support is early and intentionally narrower than the POSIX and Linux backends; MSYS2 remains the current Windows hosted bootstrap environment
- The abstraction is intentionally small; the shared runtime now has a targeted task-pool and I/O-loop proposal where needed, but there is still no broad pthread-style userland threading layer
- Networking and TLS support are practical but still narrower than a full libc, OpenSSL, or shell environment

## SEE ALSO

man, project-layout, runtime, threading, build, macos, userland
