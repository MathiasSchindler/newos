# PLATFORM

## NAME

platform - the OS abstraction layers for hosted and freestanding builds

## DESCRIPTION

The platform component keeps shared code away from direct OS calls. Runtime and
tool code are expected to program against `src/shared/platform.h`, which is then
backed by either the hosted POSIX implementation or the freestanding raw-Linux
syscall implementation.

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

## CONTRIBUTOR BOUNDARIES

- Shared code should call `platform_*` helpers rather than `open(2)`,
  `read(2)`, `getenv(3)`, or other OS APIs directly.
- If a new capability is needed in both build modes, add it to the platform
  interface and implement both backends.
- ABI- or startup-specific code belongs under `src/arch/*`, not in the generic
  runtime or tool sources.

## LIMITATIONS

- Hosted development assumes a POSIX environment; the true freestanding target currently focuses on Linux/AArch64 and Linux/x86-64
- On macOS, the project currently favors local hosted binaries over a separate Darwin syscall-only userland target
- The abstraction is intentionally small; there is no threading or async event layer
- Networking and TLS support are practical but still narrower than a full libc, OpenSSL, or shell environment

## SEE ALSO

man, project-layout, runtime, build, macos, userland
