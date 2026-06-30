# Benchmark suite

This directory contains host-side performance comparisons for selected newos tools against the standard utilities available on the development machine.

## Scope

The benchmarks currently cover:

- sort
- md5
- sha256
- sha512
- gzip
- bzip2
- xz when the host provides it
- Git developer hot paths: porcelain status, `ls-files -z`, diff name output,
  and the `diff --quiet` dirty-worktree fast path
- Git large-fixture hot paths via `git_large_benchmark.py`, which generates a
  deterministic local repository under `tests/tmp/benchmarks/git-large`
- Opt-in remote Git HTTPS benchmarks via `git_remote_benchmark.py`

## Usage

Run the suite from the repository root with:

make benchmark

or directly with:

./tests/benchmarks/run_benchmarks.sh

For a larger Git-only fixture without depending on network clones:

```sh
python3 tests/benchmarks/git_large_benchmark.py --recreate
```

Set `NEWOS_GIT=/path/to/git` to benchmark a non-default in-tree Git binary, or
adjust fixture size with `--files`, `--dirs`, `--commits`, `--dirty-files`, and
`--untracked-files`.

For opt-in remote HTTPS checks against external servers:

```sh
NEWOS_GIT="$(pwd)/build/host-linux-x86_64/git" \
  python3 tests/benchmarks/git_remote_benchmark.py \
  --repo https://github.com/octocat/Hello-World.git \
  --scenarios clone-shallow,clone-filtered,fetch-noop-shallow
```

Remote benchmarks alternate in-tree and canonical Git runs, write CSV results to
`tests/tmp/benchmarks/git-remote/results.csv`, and record basic compatibility
metadata such as checked-out HEAD, clean status, and tracked file count. Clone
scenarios also emit post-clone local timings for `rev-parse HEAD`,
`status --short`, and `ls-files` so transport time can be separated from local
metadata costs.

Set `NEWOS_GIT_HTTP_TRACE=1` on an individual in-tree Git run to print coarse
smart-HTTP request timings to stderr. The trace reports connect/TLS, request
write, response-header wait, response-body read, total elapsed time, status, and
body bytes for each Git HTTP request; this is useful when comparing remote
benchmark ratios because the local post-clone rows are often near noise-floor
timings while HTTPS round trips dominate.

The local Git hot-path benchmark is also useful for tracking individual
metadata-path changes. The in-tree Git keeps the default cached `ls-files`
listing on a streaming index parser and buffered writer, uses metadata-only
commit tree loading for status and name/stat commit comparisons, skips HEAD
tree expansion when the index cache-tree root already matches HEAD, collapses
default status output for wholly untracked directories, avoids full untracked
subtree collection when no ignore patterns are active, and reuses cached
worktree stat data when a later diff/stat step needs the same path. Single
packed-object reads use the pack index for direct non-delta object lookup before
falling back to full pack parsing.

## Notes

- benchmark scratch data is created under tests/tmp/benchmarks
- results are comparative and best used for trend tracking
- remote Git benchmarks are opt-in because external server load, routing,
  caching, TLS setup, and rate limits add noise; use medians and repeated
  alternating runs rather than single timings
- for Git hot-path investigation, pair elapsed benchmarks with `strace -c` for
	syscall shape and `PROFILE=1`/`NEWOS_PROFILE` plus `profiler` for CPU hot paths
- `status` and worktree `diff --name-status` remain heavily shaped by per-entry
  worktree metadata checks; compare syscall counts as well as elapsed time when
  evaluating changes in this area
- some newos tools intentionally implement a smaller feature set than the system tools, so the numbers are useful as engineering guidance rather than exact product-style rankings
