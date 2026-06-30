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

## Notes

- benchmark scratch data is created under tests/tmp/benchmarks
- results are comparative and best used for trend tracking
- for Git hot-path investigation, pair elapsed benchmarks with `strace -c` for
	syscall shape and `PROFILE=1`/`NEWOS_PROFILE` plus `profiler` for CPU hot paths
- some newos tools intentionally implement a smaller feature set than the system tools, so the numbers are useful as engineering guidance rather than exact product-style rankings
