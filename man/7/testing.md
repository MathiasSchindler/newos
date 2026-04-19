# TESTING

## NAME

testing - smoke-test and benchmark workflow for the project

## DESCRIPTION

The repository relies on shell-script smoke tests rather than a unit-test
framework. `make test` builds the hosted binaries, runs the suites in parallel,
and summarizes the result. This is the main correctness check contributors are
expected to use before sending changes.

## STRUCTURE

- `tests/run_smoke_tests.sh` — main runner; starts all suites and reports a
  combined status
- `tests/lib/assert.sh` — shared assertion helpers
- `tests/suites/` — area-specific smoke suites
- `tests/benchmarks/` — performance comparisons against host-system tools
- `tests/tmp/` — scratch output and per-suite logs; safe to delete

## SUITES

- `core_tools.sh` — basic Unix-style utilities
- `extended_tools.sh` — richer text, archive, and filesystem behavior
- `shell.sh` — `sh` parsing, built-ins, and script execution
- `compiler.sh` — `ncc` behavior and compile flows
- `boundaries.sh` — edge conditions and platform-sensitive cases

## CONTRIBUTOR NOTES

- A failing run leaves logs under `tests/tmp/logs/<suite>.log`; start there
  rather than rerunning blindly.
- When changing a tool, extend the nearest existing suite instead of creating a
  one-off runner.
- Benchmarks are informational; smoke tests are the main regression gate.
- `make test` validates the hosted build, so changes in shared runtime,
  platform, startup, or compiler-link logic should still be followed by a
  `make freestanding` check.

## HOSTED VS FREESTANDING VALIDATION

The hosted test path is the primary contributor workflow because it is fast and
exercises most user-visible behavior. The freestanding path is a second check:
it is not the same as the smoke-test run, and it can still fail when the hosted
suites are green.

A good rule of thumb is:

- tool behavior change: run `make test`
- low-level runtime or platform change: run `make test` and `make freestanding`
- compiler or startup-path change: run both and inspect the compiler suite

## LIMITATIONS

- The automated suite validates the hosted build only
- Tests are black-box shell scripts, so fine-grained unit isolation is limited
- Benchmarks assume the host system provides comparable reference tools

## SEE ALSO

man, project-layout, build, shell, compiler
