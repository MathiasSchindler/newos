# A Freestanding Software Foundry

This project is a freestanding software foundry: a source tree for building small command-line programs and the shared low-level machinery they need, without relying on external package dependencies or a standard C library in the target environment.

The word "foundry" is used here in a technical sense. The project is not only a set of finished programs. It is also the environment in which new programs are shaped. When a new tool needs a reusable capability, such as terminal UI support, TLS, Unicode handling, archive parsing, XML processing, or platform access, that capability is added to the project itself and then becomes available to other tools. The tools and the shared layers are expected to evolve together.

This document explains the model to a reader who has not seen the repository before. It is also intended as guidance for an LLM or other code generator that is asked to add new software to the project.

## What The Project Contains

The repository contains a growing Unix-style userland written in C. It includes many command-line tools, a shell, a self-hosting C compiler named `ncc`, shared runtime code, platform backends, tests, benchmarks, and in-tree manual pages.

The tools cover common areas such as file operations, text processing, process inspection, networking, archive handling, compression, hashing, XML processing, image metadata inspection, simple services, and build support. The goal is not to clone every option of GNU, BSD, POSIX, or BusyBox tools. The goal is to provide practical tools that work inside this project and can be built in hosted and freestanding modes.

The current manuals under `man/7` are part of the design documentation. Important overview pages include:

- `man/7/project-layout.md` for repository structure and ownership boundaries
- `man/7/platform.md` for hosted and freestanding platform layers
- `man/7/runtime.md` for shared libc-independent runtime support
- `man/7/build.md` for build modes and target selection
- `man/7/testing.md` for validation workflow
- `man/7/userland.md` for the current command surface and missing system pieces
- `man/7/unicode.md` for terminal text, UTF-8, width, and display behavior
- `man/7/shell.md` and `man/7/compiler.md` for the shell and compiler subsystems

Read those pages before making broad architectural changes.

## Freestanding Means No Target Libc

In this project, "freestanding" means that the target Linux build does not depend on a standard C library. Freestanding binaries use a small startup path, direct Linux syscalls through project-owned architecture glue, and project-owned runtime helpers.

The hosted build still exists, but it is no longer the design center. It is used for POSIX verification, fast debugging, and platform bring-up before native code exists. Hosted binaries use the POSIX platform backend. The freestanding Linux build uses the Linux syscall backend and architecture-specific startup files, while the macOS project-linked build uses the Darwin platform layer, the in-tree Mach-O linker, and no intended `libSystem` or other dylib imports.

The important rule is that ordinary tool logic should not care which backend is active. Tool code should depend on the shared runtime and the platform interface, not on libc, POSIX calls, or host-specific headers directly.

## Why This Shape Exists

Modern software often grows by adding external dependencies. That can be useful, but it also creates transitive dependency chains, hidden runtime assumptions, larger binaries, and APIs that may be difficult to inspect or change.

This project takes the opposite approach. A new program is expected to be built from local source files, local shared primitives, and narrow platform interfaces. If the program needs a capability that is missing, the project can absorb that capability into `src/shared`, `src/platform`, or a tool-private module, depending on how broadly it applies.

This makes the repository especially suitable for generating small static tools because the available building blocks are explicit and local. A code generator does not need to choose a package manager, framework, or external SDK. It should inspect the existing project layers, reuse the local primitives, and add only the missing pieces that fit those layers.

## Stability Model

The project does not treat internal interfaces as stable public APIs. Shared helpers, platform functions, runtime facilities, and tool-private modules may move or change when the repository evolves.

This is intentional. The tools live in the same source tree as the shared layers so they can be updated together. A tool is not expected to vendor an old copy of an interface or remain compatible with an old internal API. It is expected to stay in the foundry and move with the foundry.

This does not mean user-visible behavior should be careless. The intended distinction is:

- internal C interfaces may change when that improves the project
- tools should keep useful command-line behavior stable where scripts or users reasonably rely on it
- behavior changes should be covered by tests when practical
- unsupported options and intentional incompatibilities should be documented rather than hidden

The repository is therefore closer to an integrated system than to a collection of independently versioned libraries.

## Repository Layers

The main source layers are:

- `src/tools/`: command entry points and tool-specific code
- `src/tools/<name>/`: private modules for a larger tool named `<name>`
- `src/shared/`: reusable code used by more than one tool or maintained as a project-wide subsystem
- `src/shared/runtime/`: libc-independent memory, string, parsing, I/O, and Unicode support
- `src/platform/posix/`: hosted POSIX backend used by secondary host builds
- `src/platform/linux/`: freestanding Linux backend using raw syscalls
- `src/platform/macos/`: project-linked Darwin backend used by local macOS/aarch64 `make freestanding`
- `src/arch/aarch64/linux/` and `src/arch/x86_64/linux/`: startup and syscall ABI glue
- `src/compiler/`: the `ncc` compiler implementation
- `man/`: in-tree manuals and design notes
- `tests/`: smoke tests, per-tool checks, integration suites, and benchmarks

Most new command-line tools start as one file under `src/tools/<tool>.c`. If the tool grows enough internal structure, keep the public entry point in `src/tools/<tool>.c` and place private modules under `src/tools/<tool>/`.

Do not place one-off helper code in `src/shared` just because it is convenient. Shared code should either be reused by more than one tool or be deliberately designed as a project-wide primitive.

## Platform Boundary

The platform boundary exists so most code can be shared by hosted and freestanding builds.

Generic tool and shared code should call project platform functions declared through `src/shared/platform.h`. It should not call operating-system APIs such as `open`, `read`, `write`, `fork`, `exec`, `socket`, `getenv`, or `clock_gettime` directly unless the code is inside the appropriate platform backend.

If a new OS-facing capability is required, add a narrow platform abstraction and implement it in the relevant backend or document why the capability is hosted-only. ABI-specific startup or syscall details belong under `src/arch/*`, not in generic tools.

The hosted backend is not the design center. It is the comparison, debugging, and bring-up path. The Linux and macOS freestanding paths are the normal pressure tests that prevent the project from becoming accidentally libc-dependent.

## Runtime And Shared Facilities

The shared runtime is not a full replacement for libc. It is a compact internal support layer for the project's tools.

Use project helpers for common low-level work:

- runtime memory and string helpers instead of libc allocation and string APIs
- runtime parsing helpers for numeric and textual parsing where available
- runtime I/O wrappers over the platform layer
- runtime Unicode and terminal text helpers for UTF-8, display width, wrapping, tabs, ANSI escape handling, incomplete input, and invalid input
- `tool_util` for common command-line parsing, diagnostics, path behavior, regex support, and copy/remove helpers where appropriate
- existing shared subsystems for compression, archive handling, hashing, crypto, TLS, XML, image metadata, TUI support, and simple daemon configuration when they match the task

Before adding a new helper, search for an existing local equivalent. If one exists, use it. If the existing helper is close but incomplete, improve it in a way that keeps current users working.

## Adding New Software

When adding a new tool, follow this general process:

1. Read the related manual pages under `man/7`, especially project layout, platform, runtime, build, testing, and any subsystem-specific page.
2. Inspect nearby tools that solve similar problems.
3. Decide whether the new code is tool-private, shared, platform-specific, or architecture-specific.
4. Add the public tool entry point under `src/tools/<tool>.c`.
5. Put private implementation files under `src/tools/<tool>/` if the tool is too large for one file.
6. Reuse `src/shared` facilities for cross-tool behavior.
7. Add or extend platform functions only when generic code needs OS access that is not already represented.
8. Add the tool to the `TOOLS` list in the root `Makefile` if it should build with the normal userland.
9. Add a manual page under `man/1` for user-facing behavior when the tool is meant to be part of the documented command set.
10. Add or extend tests under `tests/phase1` or `tests/suites`, depending on whether the behavior is per-tool or workflow-level.
11. Run the relevant build and test targets.

For most ordinary tools, the expected build path is handled by the generic Makefile rules once the source file exists and the tool is listed in `TOOLS`.

## Guidance For LLM Code Generation

An LLM asked to build software in this repository should treat the repository as the available framework. It should not assume access to libc, external packages, generated bindings, system SDKs, or third-party libraries for target behavior.

The correct behavior is to inspect and extend the local codebase.

Before writing code, the LLM should identify:

- which existing tool is closest in shape
- which shared subsystem already solves part of the problem
- whether the code must work in both hosted and freestanding builds
- whether any new OS-facing primitive belongs in `src/platform`
- whether any new reusable logic belongs in `src/shared`
- whether the feature is risky enough to require focused malformed-input tests

The LLM should prefer small, direct C code over abstraction-heavy designs. It should not introduce dependency managers, build systems, vendored libraries, generated framework code, or host-only shortcuts.

If a new feature needs a reusable capability, the LLM should add the smallest coherent local primitive that fits the foundry. For example, if a mail-oriented tool needs terminal interaction, shared terminal UI support may belong in `src/shared/tui.*`; if several tools need the same text-display behavior, it should use or extend the runtime Unicode and text segment helpers; if a network tool needs TLS behavior, it should use or extend the existing shared TLS and platform TLS layers.

The LLM should keep user-visible behavior practical and tested. It does not need to implement every historical option of a standard Unix tool unless the request specifically requires that compatibility.

## What Not To Do

Do not add direct libc or POSIX calls to generic tool code when a platform abstraction is appropriate.

Do not add external dependencies for target functionality.

Do not place tool-private code in `src/shared` unless it is genuinely reusable or intentionally becoming a shared subsystem.

Do not make a hosted-only implementation silently appear portable. If a behavior only works in hosted builds, document that limitation.

Do not treat current internal helper names or signatures as public API contracts. Change them when needed, but update all in-tree users and tests.

Do not overfit new tools to GNU behavior unless GNU compatibility is the explicit goal. Practical behavior, clear errors, and project coherence matter more than complete option parity.

## Build And Test Expectations

The normal freestanding-oriented development loop is:

```sh
make freestanding
make test
```

On Linux, `make freestanding` builds the raw-syscall target. On local macOS/aarch64, `make freestanding` builds the project-linked no-libSystem Mach-O target. Use `make host` when you specifically need the POSIX comparison build, faster hosted diagnostics, or a temporary path while native platform code is being added.

For low-level runtime, platform, compiler, startup, or shared subsystem changes, run the broadest practical test target. The self-hosting check is also important when changes may affect the compiler or the project's ability to rebuild itself:

```sh
make selfhost
```

Tests are shell-based and live under `tests`. Per-tool behavior usually belongs in `tests/phase1`. Larger workflows belong in `tests/suites`. Benchmarks live under `tests/benchmarks` and are useful for observing performance changes, but correctness tests are the main gate.

## Security And Reliability Posture

Small static binaries are not automatically secure or correct. This project reduces some classes of complexity by avoiding external runtime dependencies, but it also owns more implementation responsibility.

Parser-heavy and network-facing code should be treated carefully. Tools and shared subsystems involving TLS, SSH, mail, XML, archives, image metadata, login, service supervision, or other hostile input surfaces need stronger tests than simple file and text filters.

Good additions should include malformed-input cases, boundary cases, and regression tests for discovered bugs. Where practical, compare behavior against existing system tools, but do not copy their behavior blindly when it conflicts with the project's documented scope.

## Summary

A freestanding software foundry is an integrated source environment for producing small, static, dependency-free programs. It provides tools, shared primitives, platform layers, build rules, tests, and manuals in one tree. New software is expected to be built inside that tree, using and improving the local layers rather than importing external dependencies.

The project accepts internal movement as part of its development model. Shared code can change because all in-tree tools can change with it. The intended result is a coherent, inspectable userland where new capabilities become part of the foundry and are available to the next program built there.