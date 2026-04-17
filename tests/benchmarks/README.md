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

## Usage

Run the suite from the repository root with:

make benchmark

or directly with:

./tests/benchmarks/run_benchmarks.sh

## Notes

- benchmark scratch data is created under tests/tmp/benchmarks
- results are comparative and best used for trend tracking
- some newos tools intentionally implement a smaller feature set than the system tools, so the numbers are useful as engineering guidance rather than exact product-style rankings
