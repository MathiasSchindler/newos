# STRACE WORKFLOWS

## NAME

strace-workflows - syscall tracing, stocktake, and syscall-level profiling

## DESCRIPTION

`strace` is a syscall tracer. In this project it is also a practical
syscall-level profiler for freestanding tools, especially on the macOS
project-linked build where the tools emit compact trace records through the
project platform layer.

This is profiling, but at a specific layer. It does not replace CPU profiling,
allocation profiling, benchmarks, or algorithmic review. It answers questions
such as:

- how many times did a tool cross into the kernel?
- which syscalls dominate a workflow?
- which calls are expected failures used as probes?
- which tools write many tiny fragments instead of batching output?
- which shared helper causes repeated path, open, readlink, mkdir, or unlink
  traffic across many tools?

Use `strace` when the cost is likely to be syscall count, syscall errors, file
system probing, output fragmentation, or platform-wrapper behavior. Use
benchmarks or a CPU profiler when the cost is likely to be parsing, crypto,
compression, matching, rendering, or other CPU work.

## BACKENDS

The Linux backend traces a child process through `ptrace`. It is the closer
model to system `strace` and can observe Linux syscalls made by the traced
process.

The macOS project-linked backend is different. Project-linked tools report
completed syscall records from selected newos platform and runtime wrappers.
This makes it useful for project tools and for test-suite stocktakes, but it is
not a general tracer for arbitrary macOS binaries or direct `svc` instructions
outside the wrappers.

The macOS backend supports raw record capture. A traced child inherits
`NEWOS_STRACE_FD`; platform wrappers write fixed-size records there; later
`strace --records FILE` replays those records as normal text, summary, or JSON
output. This separation is important because it lets a test suite run normally
while records are collected out of band.

## MODES

For one command, use normal `strace`:

```sh
./build/macos-aarch64/strace -c ./build/macos-aarch64/pgpmsg inspect message.pgp
./build/macos-aarch64/strace -e open,read,write,close ./build/macos-aarch64/tar -tf archive.tar
```

Use `-c` for a compact syscall table. The `bytes` column is meaningful only for
byte-returning calls such as `read` and `write`; it is not a sum of file
descriptors or success codes from unrelated syscalls.

Use `-e` to narrow the trace before inspecting details. Good first filters are:

```sh
-e open,close,stat,lstat,readlink
-e read,write
-e mkdir,unlink,rename
```

Use `--records FILE` when replaying a raw macOS project-linked capture:

```sh
./build/macos-aarch64/strace -c --records tests/tmp/records.bin
./build/macos-aarch64/strace --records tests/tmp/records.bin
```

## PHASE 1 STOCKTAKE

The recommended suite-wide workflow is:

```sh
make stocktake-strace-phase1
```

This reuses the existing Phase 1 correctness tests as representative scenarios.
It avoids maintaining a separate tracing scenario list and keeps trace data tied
to real behavior that already has assertions.

The output lives under `tests/tmp/strace-phase1-stocktake/`:

- `groups.tsv` records each Phase 1 group, traced status, baseline status,
  record size, and log paths
- `syscall-summary.tsv` records per-group syscall counts, errors, byte totals,
  and total time
- `aggregate-by-syscall.tsv` sums syscall usage across all groups
- `hotspots.tsv` highlights ratios and error-heavy rows worth inspecting
- `raw/`, `summaries/`, `logs/`, and `baseline-logs/` hold the underlying data

Use `PHASE1_FILTER` for focused work:

```sh
make stocktake-strace-phase1 PHASE1_FILTER=tools/text/ripgrep
```

By default the stocktake runner sets `NEWOS_STRACE_FILTER=default` and
`NEWOS_STRACE_NO_METADATA=1`. The default filter captures open, close, stat,
lstat, readlink, mkdir, unlink, rename, and related path operations while
skipping read/write. This keeps exact-output tests much less likely to be
perturbed by tracing. Metadata is disabled so the tracing path does not add
extra timing and pid syscalls to every event.

For a focused full-capture pass, use:

```sh
NEWOS_STRACE_STOCKTAKE_FILTER=all make stocktake-strace-phase1 PHASE1_FILTER=tools/system/pgpmsg
```

Full capture is best for one or a few groups. It is useful when diagnosing
read/write fragmentation, but it is noisier and more likely to perturb fragile
exact-output tests.

## INTERPRETING COUNTS

High call counts are not automatically bugs. Treat the table as a lead list.
Useful categories are:

- repeated successful calls: possible batching or caching opportunity
- repeated failing calls: possible probe-before-check pattern
- high write calls with low bytes: fragmented output
- many open errors: expected search behavior or avoidable missing-file probes
- many mkdir/unlink errors: cleanup or create-if-missing logic that may be using
  the failing syscall as its existence test
- many lstat calls: directory walking, search paths, or repeated canonicalization

Always inspect a raw replay before changing code:

```sh
./build/macos-aarch64/strace --records tests/tmp/strace-phase1-stocktake/raw/tools_text_ripgrep.records
```

The raw lines show whether a count is one bad loop, normal search behavior, or a
suite deliberately testing errors.

## BASELINE FAILURES

Stocktake runs can expose normal Phase 1 failures for the selected build tree.
The runner therefore reruns failed groups without tracing by default and records
both statuses in `groups.tsv`.

A traced failure with a passing baseline is a tracing bug or trace perturbation.
A traced failure with the same baseline failure is still useful for reports, but
it should not be treated as a tracing regression.

This distinction matters on macOS project-linked tools, where some Phase 1
groups are not viable on every local build path and where tracing itself must be
kept conservative.

## STRATEGIES THAT WORK

Start broad, then narrow. Run the full stocktake to see the shape of the whole
toolbox. Then focus on one group or syscall family with `PHASE1_FILTER` and
`-e` filters.

Prefer fixing root shared helpers when one syscall pattern appears across many
tools. Repeated `readlink` failures in path canonicalization are a shared-helper
problem; fixing one tool at a time would hide the common cause.

Use mode or directory-entry information before using failing syscalls as probes.
If code already has `lstat` results or directory entries, use them to decide
whether `readlink`, `open`, or `mkdir` is worth calling.

Do not remove intentional probing blindly. `which` searching `PATH`, `ripgrep`
checking ignore-file names, and tests for missing files can legitimately create
errors. Optimize only when the code already has enough information to avoid the
failed call without changing semantics.

Batch output when the summary shows many writes for few bytes. ASCII armor is a
good example: writing one base64 character at a time creates thousands of
syscalls; buffering one armor line preserves output while reducing kernel
crossings. The same idea applies to table renderers, JSON emitters, and tools
that print many small fields.

Treat `man` separately. It is often the largest syscall-volume source because
manual lookup and keyword search walk roots and sections repeatedly. Improvements
there are likely to involve root caching, section ordering, or search indexing,
so they should be reviewed as behavior changes rather than incidental cleanup.

## VALIDATION

After changing a traced pattern, run both behavior tests and a focused stocktake.
For example:

```sh
make build/macos-aarch64/ripgrep build/macos-aarch64/strace
NEWOS_TEST_BUILD_DIR="$PWD/build/macos-aarch64" \
  PHASE1_JOBS=1 sh ./tests/phase1/run_phase1_tests.sh tools/text/ripgrep
NEWOS_TEST_BUILD_DIR="$PWD/build/macos-aarch64" \
  sh ./scripts/stocktake-strace-phase1.sh tools/text/ripgrep
```

For shared path, runtime, or output changes, follow with the broader gates:

```sh
make freestanding
make host
make stocktake-strace-phase1
```

A stocktake improvement is not a substitute for correctness tests. It is a way
to decide where to look, verify that a targeted syscall pattern changed, and
catch new trace-induced regressions.

## CAUTIONS

Tracing changes execution. The macOS backend therefore defaults to filtered,
no-metadata capture for suite stocktakes. Use full capture only when the added
read/write records are relevant.

Do not compare timing numbers from default stocktake output; `total_ms` is zero
by design when metadata is disabled. For timing, run a focused trace with timing
enabled or use the benchmark suite.

Do not optimize only for fewer syscalls if it makes code less correct or less
portable. A few expected `ENOENT` probes can be the right behavior for PATH
search, feature detection, or security checks. The best fixes remove accidental
work while preserving the same observable behavior.

## SEE ALSO

strace(1), testing(7), build(7), runtime(7), platform(7)
