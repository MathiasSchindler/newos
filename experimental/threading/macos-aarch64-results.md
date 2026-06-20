# macOS AArch64 Results, 2026-06-20

Command:

```sh
make -C experimental/threading report
```

Host summary:

- Host: Darwin arm64
- Host CPUs reported by `sysctl`: 16 logical, 16 physical
- Runtime worker substrate: `workers_supported=1 detected_width=16`

## Conclusion

The macOS/aarch64 project-linked build now creates native worker threads and the
shared task pool can use many cores. Requested widths 2, 4, 8, and 16 all report
matching effective widths.

The CPU-bound `mix` workload scales well through 16 workers, reaching about 12x
speedup on this run. The `memory` workload improves through 8 workers and then
flattens, which is consistent with memory bandwidth becoming the limit. The tiny
`tasks` and `overhead` cases are slower with more workers; they are useful as
stress tests for dispatch overhead, not as examples of profitable parallelism.

## Width Sweep

Representative rows from the final report:

```text
case,requested_width,effective_width,units,min_chunk,best_ns,ns_per_unit,units_per_sec,speedup,checksum
mix,1,1,1048576,4096,365689417,348.74,2867394,1.00,17944356023320485625
mix,2,2,1048576,4096,190945541,182.09,5491492,1.91,5549128295428774134
mix,4,4,1048576,4096,94754542,90.36,11066234,3.85,1966161435201784660
mix,8,8,1048576,4096,48425750,46.18,21653273,7.55,6529123515222484245
mix,16,16,1048576,4096,30287625,28.88,34620608,12.07,5957877188118077011
memory,1,1,1048576,4096,3344583,3.18,313514719,1.00,12122385442549333595
memory,8,8,1048576,4096,566417,0.54,1851243871,5.90,5449263094134372991
memory,16,16,1048576,4096,618833,0.59,1694440988,5.40,3073708585775754884
tasks,1,1,65536,1024,1555250,23.73,42138562,1.00,14567404763593168068
tasks,16,16,65536,1024,4602334,70.22,14239731,0.33,13116042221729256279
overhead,1,1,262144,1,265625,1.01,986895058,1.00,9900109334497896719
overhead,16,16,262144,1,94981292,362.32,2759954,0.00,1734111747028792859
```

## Chunk Sweep

For a smaller CPU workload with 262,144 items and 16 mix rounds, tiny chunks are
still expensive. Useful scaling appears once chunks have enough work to amortize
wakeups and atomic chunk claims.

```text
chunk=1     mix,16,16,262144,1,141553541,539.98,1851907,0.04,...
chunk=64    mix,16,16,262144,64,792542,3.02,330763543,7.05,...
chunk=512   mix,16,16,262144,512,503834,1.92,520298352,10.75,...
chunk=4096  mix,16,16,262144,4096,547291,2.08,478984671,9.70,...
chunk=32768 mix,16,16,262144,32768,726500,2.77,360831383,7.68,...
```

The practical rule is unchanged but now visible in parallel: avoid single-item
chunks unless each item is much heavier than this synthetic mix body.

## Workload Size Sweep

For the CPU mix case with 64 rounds and `min_chunk=4096`:

```text
items=4096    mix,16,16,4096,4096,563500,137.57,7268855,1.10,...
items=65536   mix,16,16,65536,4096,1295916,19.77,50571178,7.76,...
items=1048576 mix,16,16,1048576,4096,12818292,12.22,81803098,12.41,...
```

Small workloads are dominated by fixed startup and dispatch costs. Larger CPU
workloads benefit strongly from the native macOS worker substrate.
