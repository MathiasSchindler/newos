# TESTING

## NAME

testing - smoke-test and benchmark workflow for the project

## DESCRIPTION

The repository relies on modular shell-script validation rather than a unit-test
framework. `make test` builds the hosted binaries, runs the Phase 1 per-tool
checks plus the remaining higher-level smoke suites, builds the freestanding
Linux binaries where supported, and runs freestanding and isolated-userland
smoke suites. This is
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
- `tests/suites/userland.sh` — isolated shell-session checks run through
  `scripts/test.sh` and `scripts/run-userland.sh`
- `tests/benchmarks/` — performance comparisons against host-system tools
- `tests/compiler/` — focused compiler checks and synthetic ncc-vs-gcc
  benchmark cases for code quality, size, and runtime tracking
- `tests/tmp/` — scratch output and per-suite logs; safe to delete

## SUITES

- `tests/phase1/run_phase1_tests.sh` — runs the canonical per-tool correctness
  suite
- `scripts/stocktake-strace-phase1.sh` — runs existing Phase 1 groups with
  macOS project-linked trace record capture enabled, then replays the records
  through `strace -c` for stocktake reports
- `extended_tools.sh` — multi-tool workflow and integration scenarios
- `freestanding.sh` — validates representative freestanding tools from
  `build/freestanding-linux-$(TARGET_ARCH)/`
- `shell.sh` — `sh` parsing, built-ins, and script execution

Current Phase 1 coverage spans the core filesystem and text utilities plus
compiler, math, platform, archive/compression, checksum, ELF inspection,
process-state, metadata, image/C2PA, XML, daemon, service, TLS-backed network,
and basic local network/build contracts.

## CONTRIBUTOR NOTES

- A failing run leaves logs under `tests/tmp/logs/<suite>.log`; start there
  rather than rerunning blindly.
- When changing a tool, extend the nearest existing Phase 1 script instead of
  creating a one-off runner.
- Reserve `extended_tools.sh` for real workflow-style tests that combine
  multiple tools.
- Phase 1 runs several per-tool scripts in parallel by default; set
  `PHASE1_JOBS=1` when debugging a single failure in strict serial order.
- Use `make stocktake-strace-phase1` to reuse Phase 1 scenarios as a syscall
  census on macOS project-linked tools. Pass `PHASE1_FILTER=tools/text/grep` for
  a focused group. Reports are written under
  `tests/tmp/strace-phase1-stocktake/`. `groups.tsv` includes both traced
  `status` and untraced `baseline_status` for failed groups, so baseline Phase 1
  failures stay visible without making the stocktake target fail. The runner
  defaults to a non-I/O capture filter (`open`, `close`, `stat`, and path
  operations) so exact output tests are not perturbed by tracing every `read`
  and `write`; set `NEWOS_STRACE_STOCKTAKE_FILTER=all` for a focused
  full-capture pass. See `man/7/strace.md` for recommended interpretation and
  optimization workflows.
- Benchmarks are informational; smoke tests are the main regression gate. Use
  `make compiler-benchmark` for synthetic compiler code-quality comparisons.
- `make test` validates both the hosted path and a representative freestanding
  path on Linux. Running `make freestanding` alone is still useful when you
  only need to check the target build.

## PERFORMANCE CHECKS

For speed work, compare both hosted and freestanding binaries against the same
fixtures, and keep the freestanding size goal visible throughout the pass. On
macOS, compact project-linked binaries are rounded in coarse pages, so an
unchanged final file size does not prove a change is size-free; also review the
source-level code and data added by the optimization.

Recent size-conscious wins came from:

- batching output with `ToolOutputBuffer` in tools that write many small fields
  or records, such as `sed` and `xmlmin`
- adding narrow fast paths for common cases, such as plain literal regex search,
  ASCII case-insensitive literal matching, and simple `.`-wildcard patterns
- increasing streaming buffers to 16 KiB where stack use remains acceptable for
  the tool, such as raw text and byte-formatting filters
- using compact middle-ground tables only when they replace much slower loops,
  such as the 16-entry CRC32 nibble table used by compression-related tools

Avoid broad speedups that trade a large table or a much bigger generic runtime
for a narrow benchmark result. Prefer benchmarked tool-local changes first, then
promote helpers to `src/shared/` only when several tools use the same pattern.

## HOSTED VS FREESTANDING VALIDATION

The hosted test path is the broadest behavior check because it is fast and
exercises most user-visible behavior. The freestanding suite is intentionally
smaller, but it verifies that representative tools still run with the raw-Linux
backend, direct syscalls, static PIE linking, the freestanding stack-guard
startup path, and any stack-protector instrumentation enabled for the build.

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
