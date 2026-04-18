# TESTING

## NAME

testing - the smoke-test and benchmark infrastructure for the project

## DESCRIPTION

The tests directory contains a structured smoke-test suite that exercises the built host-side tools, a shared assertion library, and a separate benchmarks area for comparing tool performance against the host system equivalents.

## STRUCTURE

- tests/run_smoke_tests.sh — entry point; runs all suites in parallel and reports results
- tests/lib/assert.sh — shared assertion helpers sourced by each suite
- tests/suites/ — individual test suites, one script per area
- tests/benchmarks/ — benchmark scripts comparing newos tools to system tools
- tests/tmp/ — temporary working directory created at test runtime; not committed

## SUITES

- core_tools.sh — tests for fundamental tools (ls, cat, cp, echo, …)
- extended_tools.sh — tests for more complex tools (sed, awk, sort, grep, …)
- shell.sh — tests for sh built-ins and scripting features
- compiler.sh — tests for ncc compilation behaviour
- boundaries.sh — edge-case and boundary-condition tests

## WORKFLOW

    make test       — build host binaries and run the full smoke-test suite
    make benchmark  — build host binaries and run the benchmark suite

The smoke-test runner launches all suites concurrently, collects per-suite logs under tests/tmp/logs, and prints a summary on completion. A non-zero exit code means at least one suite reported a failure.

## LIMITATIONS

- Tests run against the host build only; the freestanding binaries are not exercised by the suite
- No unit test framework is present; tests are shell scripts calling the tools directly
- The benchmark suite requires the host system to provide the reference tools (coreutils equivalents)

## SEE ALSO

man, project-layout, build, shell, compiler
