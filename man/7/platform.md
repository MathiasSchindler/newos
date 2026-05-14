# PLATFORM

## NAME

platform - the OS abstraction layers for hosted and freestanding builds

## DESCRIPTION

The platform component keeps shared code away from direct OS calls. Runtime and
tool code are expected to program against `src/shared/platform.h`, which is then
backed by the hosted POSIX implementation, the freestanding raw-Linux syscall
implementation, or the native Windows freestanding PE implementation.

Windows is being treated as a new contributor workstation environment first.
Hosted Windows builds still run inside MSYS2 and use the POSIX backend, while
`make freestanding-windows` uses `src/platform/windows/` to build native PE
executables without the MSYS POSIX runtime or the Microsoft C runtime.

## BUILD-MODE SUMMARY

The platform abstraction exists so that most tool code can stay the same across
both development and target builds.

In practice:

- the hosted build is for fast local iteration, testing, and debugging
- the freestanding build is for verifying that the same logic still works with
  the raw Linux ABI and minimal startup support
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

Used by `make host` and by the smoke tests. On macOS this is also the default
local build path used by `make` and, by default, `make freestanding`.

- `fs.c` — file, directory, and path operations
- `process.c` — spawn, wait, signal, terminal, and environment handling
- `identity.c` — users, groups, sessions, and related lookups
- `net.c` — sockets plus helpers for tools such as `ping` and `netcat`
- `time.c` — clocks, formatting, uptime, and sleep helpers
- `tls.c` — platform-facing TLS client glue backed by the shared TLS code

### Freestanding Linux layer (`src/platform/linux`)

Used by `make freestanding`. This layer talks to the Linux kernel ABI directly
and does not depend on libc.

- `fs.c`, `process.c`, `identity.c`, `net.c`, `time.c`, and `tls.c` mirror the
  hosted interface using syscalls and freestanding shared code
- `stack_guard.c` — stack-canary guard initialization and failure handling for
  freestanding binaries when compiler instrumentation is enabled

### Architecture glue (`src/arch/*/linux`)

- `crt0.S` — minimal process startup for the freestanding binaries
- `syscall.h` — inline syscall helpers for the selected Linux target ABI
- `syscall_stubs.S` — x86-64 out-of-line syscall entry helpers used by the freestanding platform layer

### Native Windows freestanding layer (`src/platform/windows`)

Used by `make freestanding-windows`. This layer links PE executables directly
against the minimal Windows system DLL imports needed by the tools and keeps
Win32 details out of shared runtime and tool code.

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
- On macOS, the project currently favors local hosted binaries over a separate Darwin syscall-only userland target
- Native Windows freestanding support is early and intentionally narrower than the POSIX and Linux backends; MSYS2 remains the current Windows hosted bootstrap environment
- The abstraction is intentionally small; there is no threading or async event layer
- Networking and TLS support are practical but still narrower than a full libc, OpenSSL, or shell environment

## SEE ALSO

man, project-layout, runtime, build, macos, userland
