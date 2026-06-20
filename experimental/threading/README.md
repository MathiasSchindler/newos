# Threading Experiment

This directory is a small benchmark sandbox for the project-owned concurrency
runtime described in `../../man/7/threading.md`.

The benchmark intentionally uses synthetic workloads. It is not trying to prove
that any real tool is faster yet; it answers simpler bring-up questions:

- does the `rt_task_pool` API run correctly on this machine?
- what effective worker width did the backend actually provide?
- does a CPU-bound workload speed up as requested width grows?
- where do task overhead and memory bandwidth stop scaling?

## Build And Run

On local macOS/aarch64, this builds the project-linked Mach-O benchmark with the
in-tree linker:

```sh
make -C experimental/threading run
```

On Linux/x86_64, the same command builds a static freestanding Linux binary. To
override the target explicitly:

```sh
make -C experimental/threading TARGET_ARCH=x86_64 run
```

The convenience wrapper accepts benchmark arguments:

```sh
sh experimental/threading/run.sh --max-width 8 --repeat 5 --items 1048576
```

For a broader local report with width, chunk, and workload-size sweeps:

```sh
make -C experimental/threading report
```

## Output

`threadbench` prints CSV-style rows:

```text
case,requested_width,effective_width,units,min_chunk,best_ns,ns_per_unit,units_per_sec,speedup,checksum
mix,1,1,1048576,4096,123456789,117.73,8493465,1.00,1483184931
mix,2,1,1048576,4096,123400000,117.68,8497374,1.00,1483184931
```

`requested_width` is what the benchmark asked for. `effective_width` is what the
runtime actually provided. On current project-linked macOS, native worker threads
are not implemented yet, so `effective_width` should stay `1`; that is expected
and confirms the serial backend is doing its job. On Linux/x86_64, the native
worker substrate should allow effective widths greater than one.

`units` is `--items` for range workloads and `--tasks` for `tasks`. `speedup` is
relative to the best width-1 time for the same case. The checksum is printed to
keep the work observable and to catch accidental behavior changes.

## Cases

- `mix`: CPU-heavy integer mixing over independent ranges. This should be the
  clearest core-scaling signal once native worker threads are available.
- `memory`: writes and reduces a large `unsigned long long` buffer. This is meant
  to expose memory bandwidth limits.
- `tasks`: submits many small independent tasks through `rt_task_group`. This is
  an overhead and scheduling test, not a best-case throughput workload.
- `overhead`: uses tiny `rt_parallel_for` chunks to show the cost of excessive
  splitting.

Useful options:

```text
--case all|mix|memory|tasks|overhead
--items N
--tasks N
--rounds N
--repeat N
--max-width N
--min-chunk N
```

For scaling experiments, keep one workload fixed and vary only `--max-width` or
`--min-chunk`. For example:

```sh
sh experimental/threading/run.sh --case mix --items 1048576 --rounds 128 --repeat 5 --max-width 16
sh experimental/threading/run.sh --case overhead --items 1048576 --min-chunk 1 --repeat 5 --max-width 16
```

The local macOS/aarch64 baseline from 2026-06-20 is summarized in
`macos-aarch64-results.md`.