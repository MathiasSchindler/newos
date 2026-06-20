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

The macOS/aarch64 project-linked build creates native worker threads and the shared task pool can use many cores. Requested widths 2, 4, 8, and 16 report matching effective widths for native work.

The CPU-bound `mix` workload scales well through 16 workers, reaching about 12x median speedup in the large width sweep. The `memory` workload plateaus around widths 8 to 16, which is consistent with memory bandwidth becoming the limit. The `tasks` workload is now much better after batched `RtTaskGroup` dispatch: tiny task fan-out improved from slower-than-serial to roughly 4x, with widths 8 and 16 close enough that the smaller near-best width is often the better practical choice.

Tiny range chunks are automatically batched by the task pool. A requested `min_chunk=1` now means "at least one item," not "one atomic claim per item." The CSV reports both `requested_min_chunk` and `effective_min_chunk` so this is visible in results.

The experimental tools include counter output (`threadbench --stats`), a stress runner (`threadstress`), a host-side report comparer (`benchcmp.sh`), and a width guidance parser (`benchguide.sh`). These live under `experimental/threading` while the threading runtime is still in bring-up mode.

## Width Sweep

Representative rows from the final report. Timing-derived columns use median time; `min_ns`, `p90_ns`, and `max_ns` are included to show spread.

```text
case,requested_width,effective_width,active_workers,units,requested_min_chunk,effective_min_chunk,min_ns,median_ns,p90_ns,max_ns,ns_per_unit,units_per_sec,speedup,checksum
mix,1,1,1,1048576,4096,131072,344426333,345815209,347997208,347997208,329.79,3032185,1.00,...
mix,8,8,8,1048576,4096,16384,48308542,48360541,48648667,48648667,46.12,21682470,7.15,...
mix,16,16,16,1048576,4096,8192,28977125,29657917,30213666,30213666,28.28,35355685,11.66,...
memory,1,1,1,1048576,4096,131072,2928084,2992000,3063292,3063292,2.85,350459893,1.00,...
memory,8,8,8,1048576,4096,16384,554708,581541,650167,650167,0.55,1803099007,5.14,...
memory,16,16,16,1048576,4096,8192,630084,662292,726792,726792,0.63,1583253308,4.51,...
tasks,1,1,1,65536,1024,1024,1516292,1534666,1667333,1667333,23.41,42703754,1.00,...
tasks,8,8,8,65536,1024,1024,332666,343167,519666,519666,5.23,190974073,4.47,...
tasks,16,16,16,65536,1024,512,349416,356667,382958,382958,5.44,183745622,4.30,...
overhead,1,1,1,262144,1,32768,125,167,417,417,0.00,1569724550898,1.00,...
overhead,16,16,16,262144,1,2048,42584,56875,61084,61084,0.21,4609125274,0.00,...
```

## Chunk Sweep

For a smaller CPU workload with 262,144 items and 16 mix rounds, requested tiny chunks are batched automatically by the runtime. Useful scaling is preserved even when a caller supplies an overly small minimum chunk size.

```text
chunk=1     mix,16,16,16,262144,1,2048,512417,551875,585709,585709,2.10,475006115,10.20,...
chunk=8     mix,16,16,16,262144,8,2048,463750,472042,477542,477542,1.80,555340414,11.24,...
chunk=64    mix,16,16,16,262144,64,2048,469583,480708,483333,483333,1.83,545328973,10.91,...
chunk=512   mix,16,16,16,262144,512,2048,471458,474875,555417,555417,1.81,552027375,11.15,...
chunk=4096  mix,16,16,16,262144,4096,4096,466208,471584,492000,492000,1.79,555879758,10.99,...
chunk=32768 mix,16,16,16,262144,32768,32768,716708,717375,719167,719167,2.73,365421153,7.12,...
```

The practical rule is more forgiving than before: callers should still choose a sensible minimum chunk, but the pool protects itself against pathological one-item claim traffic.

## Workload Size Sweep

For the CPU mix case with 64 rounds and requested `min_chunk=4096`:

```text
items=4096    mix,16,16,1,4096,4096,4096,616375,617000,650709,650709,150.63,6638573,0.95,...
items=65536   mix,16,16,16,65536,4096,4096,1299875,1317458,1319583,1319583,20.10,49744280,7.66,...
items=1048576 mix,16,16,16,1048576,4096,8192,12695125,12757542,12787958,12787958,12.16,82192635,12.40,...
```

Small workloads are dominated by fixed dispatch costs and may run with `active_workers=1` even when the pool width is larger. Larger CPU workloads benefit strongly from the native macOS worker substrate.

## Counter Sample

`threadbench --stats` prints one `# stats` line after each CSV row. For a 262,144 item mix workload with requested `min_chunk=1`, the 16-worker row shows that automatic batching used 128 chunks of 2,048 items each instead of 262,144 single-item claims. It also exposes per-worker chunk imbalance:

```text
mix,16,16,16,262144,1,2048,458958,467250,479750,479750,1.78,561035848,11.23,...
# stats case=mix requested_width=16 effective_width=16 active_workers=16 dispatches=1 parallel_dispatches=1 group_dispatches=0 serial_parallel=0 serial_group=0 chunk_attempts=163 chunks=128 group_batches=0 group_tasks=0 worker_waits=15 worker_wakes=2 join_waits=1 worker_completions=16 count=262144 requested_min_chunk=1 effective_min_chunk=2048 group_batch_size=0 worker_chunks_total=128 worker_chunks_min=3 worker_chunks_max=10 worker_group_tasks_total=0 worker_group_tasks_min=0 worker_group_tasks_max=0
```

## Stress Smoke

The report also runs a randomized create/run/destroy stress pass:

```text
iterations=32 failures=0 forced_errors_seen=3 max_width=16 items=65536 rounds=4
elapsed_ns=9963458 total_pool_ns=1849586 total_parallel_ns=1561331 total_group_ns=739210
chunks=1910 chunk_attempts=2888 group_tasks=70496 worker_waits=520 worker_wakes=132 join_waits=123
```

## Width Guidance

`benchguide.sh` parses the width-sweep sections and reports the fastest width and the smallest width within 5 percent of the best median time:

```text
section,case,best_requested_width,best_active_workers,best_median_ns,best_speedup,near_best_width,near_best_active_workers,near_best_median_ns,note
width-sweep-memory,memory,16,16,617250,4.80,8,8,618417,memory-like plateau; smaller width is near-best
width-sweep-overhead,overhead,1,1,167,1.00,1,1,167,dispatch overhead floor; inspect absolute ns
width-sweep-cpu,mix,16,16,29335708,12.29,16,16,29335708,best width is also near-best width
width-sweep-tasks,tasks,16,16,363791,4.31,8,8,373000,near-best at lower width
```
