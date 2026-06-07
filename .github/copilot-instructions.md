# Copilot instructions for newos

## Overall objective

The primary objective is to produce statically linked, dependency-free, no-libc binaries. Treat `make freestanding` as the default build path on both Linux and macOS; hosted POSIX builds are secondary verification and bring-up paths.

## Build and test commands

- Use `make freestanding` by default for normal builds and validation. On Linux this builds static, libc-free Linux ABI tools under `build/freestanding-linux-$(TARGET_ARCH)/`; on local macOS/aarch64 it builds the dependency-free project-linked Mach-O tool tree under `build/newlinker-macos-aarch64/`.
- Use `make host` for the secondary hosted POSIX build and quick iteration. It writes `build/host-<os>-<arch>/` and compatibility symlinks in `build/`.
- Use `make test` as the broad regression gate. It runs hosted tests and, where supported, representative freestanding and isolated-userland smoke tests.
- Run Phase 1 per-tool checks with `make test-phase1`; run the smoke suites without Phase 1 with `make test-smoke`.
- To run one Phase 1 test after building host tools, use the Phase 1 runner filter, for example:

  ```sh
  make host
  PHASE1_JOBS=1 sh ./tests/phase1/run_phase1_tests.sh tools/text/grep
  ```

- On Linux, use `make test-freestanding` for freestanding binary smoke checks and `make test-userland` for the isolated userland shell suite.
- Use `make selfhost` after `make host` when checking `ncc`, shared runtime, shell support, or low-level dependency changes.
- Use `make benchmark` for informational host-side performance comparisons.
- Use `make experimental-multicall` and `make test-experimental-multicall` for the experimental all-tools multicall binary.
- For Windows native no-CRT PE bring-up, run `.\tests\windows\build-windows-freestanding.ps1` from PowerShell.

## Architecture

- This is a freestanding-first C userland for a Linux-ABI-compatible operating system. Hosted POSIX builds are kept as a fast verification and bring-up path, not as the primary design target.
- The active documentation source is `man/`: command manuals live in `man/1`, and current design/contributor notes live in `man/7`. Prefer `man/7/project-layout.md`, `man/7/build.md`, `man/7/testing.md`, `man/7/runtime.md`, and `man/7/userland.md` over older planning notes.
- User-facing commands live under `src/tools/`. Each command has one public entry file at `src/tools/<name>.c`; larger commands keep private modules under `src/tools/<name>/`.
- Reusable, libc-independent support code lives in `src/shared/`, including runtime helpers, tool utilities, compression, crypto/TLS, image, XML, archive, bignum, TUI, and other cross-tool primitives.
- Platform and architecture code is intentionally thin and isolated: hosted POSIX code is in `src/platform/posix/`, Linux syscall code in `src/platform/linux/` plus `src/arch/*/linux/`, macOS project-linked code in `src/platform/macos/` plus `src/arch/aarch64/macos/`, and Windows no-CRT code in `src/platform/windows/`.
- `src/compiler/` contains the `ncc` compiler, frontend/IR/backends/object writers, and the in-tree linker used by freestanding paths. Other tools should not grow hard dependencies on compiler internals.
- `src/compiler/source_manifest.h` is the canonical source-group registry. The Makefile and `ncc` driver both consume it, so add/remove shared, compiler, crypto, TLS, image, XML, shell, and other grouped source files there rather than duplicating lists in multiple places.
- Tests are shell-script based. Primary per-tool coverage is under `tests/phase1/`; higher-level smoke suites are under `tests/suites/`; logs and scratch output go under `tests/tmp/`.

## Project-specific conventions

- Keep generic tool logic independent of direct OS headers and route OS interaction through `platform.h` and the appropriate `src/platform/*` layer.
- Preserve the static/no-libc goal for freestanding outputs. Avoid adding standard-library assumptions to shared runtime or tool code unless they are confined to the hosted POSIX backend.
- Do not preserve legacy patterns or compatibility surfaces by default. The current user base is one person, and supported tools are all in `src/tools/`, so no external consumers depend on internal function locations or feature shapes. When a better approach is found by a meaningful metric, use it and rewire existing tools to the improved pattern.
- Put code in `src/shared/` only when it is genuinely reused by multiple tools or is an intentional reusable subsystem. Tool-private helpers belong under `src/tools/<tool>/`.
- When adding a new ordinary tool, add `src/tools/<name>.c` and include the tool in the `TOOLS` list in the root `Makefile`; special-dependency tools should follow existing explicit-rule patterns such as `sh`, `ncc`, `ssh`, XML, TLS, image, archive, or TUI tools.
- If shared runtime helpers or grouped source files change, keep `src/compiler/source_manifest.h` in sync because the Makefile derives several source groups from it.
- A hosted build passing does not prove the freestanding target is healthy. Validate platform/runtime/startup/linker changes against the relevant freestanding path.
- The Linux freestanding and macOS project-linked outputs use compact executable layouts. For inspection, prefer project tools such as `file`, `readelf`, `objdump`, `nm`, `size`, and `imgcheck`, which understand the project's compact ELF and Mach-O shapes.
- Phase 1 tests can run in parallel; use `PHASE1_JOBS=1` when debugging one failure or when deterministic serial output matters.
