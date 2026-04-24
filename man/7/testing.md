# TESTING

## NAME

testing - smoke-test and benchmark workflow for the project

## DESCRIPTION

The repository relies on modular shell-script validation rather than a unit-test
framework. `make test` builds the hosted binaries, runs the Phase 1 per-tool
checks plus the remaining higher-level smoke suites, builds the freestanding
Linux binaries where supported, and runs a freestanding smoke suite. This is
the main correctness check contributors are expected to use before sending
changes.

## STRUCTURE

- `tests/phase1/` — primary per-tool correctness suite, grouped by tool area
- `tests/run_smoke_tests.sh` — smoke runner for the higher-level integration and
  shell suites; it also includes Phase 1 unless explicitly skipped
- `tests/lib/assert.sh` — shared assertion helpers
- `tests/suites/` — remaining non-Phase-1 suites
- `tests/suites/freestanding.sh` — libc-free binary smoke checks, including a
  static-PIE shape check and an `httpd`/`wget` loopback round trip
- `tests/benchmarks/` — performance comparisons against host-system tools
- `tests/tmp/` — scratch output and per-suite logs; safe to delete

## SUITES

- `tests/phase1/run_phase1_tests.sh` — runs the canonical per-tool correctness
  suite
- `extended_tools.sh` — multi-tool workflow and integration scenarios
- `freestanding.sh` — validates representative freestanding tools from
  `build/freestanding-linux-$(TARGET_ARCH)/`
- `shell.sh` — `sh` parsing, built-ins, and script execution

Current Phase 1 coverage spans the core filesystem and text utilities plus
compiler, math, platform, archive/compression, checksum, ELF inspection,
process-state, metadata, and basic local network/build contracts.

## CONTRIBUTOR NOTES

- A failing run leaves logs under `tests/tmp/logs/<suite>.log`; start there
  rather than rerunning blindly.
- When changing a tool, extend the nearest existing Phase 1 script instead of
  creating a one-off runner.
- Reserve `extended_tools.sh` for real workflow-style tests that combine
  multiple tools.
- Phase 1 runs several per-tool scripts in parallel by default; set
  `PHASE1_JOBS=1` when debugging a single failure in strict serial order.
- Benchmarks are informational; smoke tests are the main regression gate.
- `make test` validates both the hosted path and a representative freestanding
  path on Linux. Running `make freestanding` alone is still useful when you
  only need to check the target build.

## HOSTED VS FREESTANDING VALIDATION

The hosted test path is the broadest behavior check because it is fast and
exercises most user-visible behavior. The freestanding suite is intentionally
smaller, but it verifies that representative tools still run with the raw-Linux
backend, direct syscalls, static PIE linking, stack canaries, and the minimal
startup path.

A good rule of thumb is:

- tool behavior change: run `make test`
- low-level runtime or platform change: run `make test`
- compiler or startup-path change: run `make test` and inspect the
  compiler-related Phase 1 coverage

## LIMITATIONS

- Tests are black-box shell scripts, so fine-grained unit isolation is limited
- Benchmarks assume the host system provides comparable reference tools

## SEE ALSO

man, project-layout, build, shell, compiler
