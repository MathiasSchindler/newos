# Medium diagnostics

The collected five-minute comparisons and graph/CPU breakdowns are in
[`../data/medium-diagnostics/long/report.md`](../data/medium-diagnostics/long/report.md).
Raw captures live alongside that report in the ignored data tree; retain them
locally when comparing future optimizations.
The nine-run collection was stopped at the user's request. Its completed runs
remain useful individual observations, not a completed repeated overhead study.

The target configuration is Whisper Medium with `fused,self,logits`. Native
instrumentation is opt-in, remains no-CRT/KERNEL32-only, and does not change
decoding policy. Python and PowerShell are development-time collectors and
analyzers, not inference dependencies.

## Capture

From the repository root, after staging the matching QNN DLLs and DSP support
files beside the isolated executable:

```powershell
.\experimental\snapdragon\tools\build.ps1 -BuildDir experimental/snapdragon/build/diagnostics-candidate
.\experimental\snapdragon\tools\profile-whisper.ps1 -Diagnostics basic -ProbePath experimental/snapdragon/build/diagnostics-candidate/npu_probe.exe -WavPath experimental/snapdragon/data/bundestag-hearing-5min-16k-mono-f32.wav -OutputDirectory experimental/snapdragon/data/medium-diagnostics/example
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe .\experimental\snapdragon\tools\analyze-whisper-diagnostics.py experimental/snapdragon/data/medium-diagnostics/example/diagnostics.csv --overview
```

Both profiling scripts now default to Medium. The single-run profiler defaults
to `fused,self,logits`; the offload benchmark compares `cross,mlp`, `fused,logits`,
and `fused,self,logits`. Quote comma-separated PowerShell parameter values.

`collect-whisper-diagnostics.ps1` runs three alternating repetitions of
uninstrumented, trace-only, and basic-profiled Medium on the five-minute fixture.
It saves executable/runtime/context/audio identities, AC/battery and power-plan
inventory, transcripts, raw counters, and thread snapshots. It rejects failed
runs, changed transcripts, QNN errors, graph failures, dropped records, and
diagnostic API failures. Existing files in the output directory are overwritten;
choose a new directory for a new comparison.

```powershell
.\experimental\snapdragon\tools\collect-whisper-diagnostics.ps1
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe .\experimental\snapdragon\tools\analyze-whisper-diagnostics.py --collection experimental/snapdragon/data/medium-diagnostics/long
```

## Modes and outputs

- No diagnostic flag: no trace buffer, event enumeration, or CPU accounting hooks.
- `--diagnostics=trace`: all graph calls and decoder phases, without device events.
- `--diagnostics=basic`: additionally sample basic QNN events on the first call
  and every 127th call of each graph. This is deterministic stratified sampling,
  not a random sample or a hardware occupancy measurement.
- `--diagnostics=detailed`: enable detailed device profiling continuously, but
  retrieve events at the same sampling interval. Use short captures for operator
  attribution, not normal-performance claims.
- `--diagnostics-output=<path.csv>`: separate CSV output, defaulting to
  `whisper-diagnostics.csv` in the current directory. Put options before the WAV.

The recorder holds at most 524,288 host intervals and 65,536 device events in
about 50 MiB of private memory. It writes buffered CSV after native cleanup,
outside reported native process time but inside wrapper wall time. File creation,
allocation, or write failures return exit 119. Ctrl+C still cleans up on the
inference thread and flushes completed trace intervals before exit 130. Overflow
is counted and rejected by the analyzer, not silently treated as complete data.

The analyzer emits:

- `diagnostics.summary.json`: per-graph/family/position-bucket median, p95, p99,
  maximum and total host-call times; inter-graph gaps; decoder phase durations;
  sampled device events and host/device differences; process CPU attributed to
  token and selection intervals; per-thread CPU/state summaries.
- `diagnostics.trace.json`: Chrome Trace Event format, viewable in a local trace
  viewer or Perfetto UI. It contains QPC-aligned token/layer/graph intervals,
  window, retry-attempt and token-position labels. Device events are durations,
  not synchronized device timestamps, so they are not drawn as fabricated device
  execution intervals.
- `threads.csv`: 250 ms snapshots of per-thread user/kernel CPU counters, state,
  and wait reason. These timestamps use the same QPC clock as the native trace.

CSV schema version 1 uses tagged rows. `graph` and `phase` rows use the header
fields literally. Phase IDs are token=0, self=1, cross/MLP=2, projection=3,
selection=4, log-mel=5. Token intervals contain their child phases. CPU time is
process-wide and collected only for token and selection intervals, in 100 ns
units; Windows accounting granularity makes individual short intervals noisy.
Do not add selection CPU to token CPU: it is already included.

`event` rows repurpose graph=host-record ID, window=parent-event ID (UINT32_MAX
for root), step=event type, position=unit, layer=value, status=API status, and
name=SDK identifier. The `meta` row maps graph=QPC frequency, window=QPC origin,
step=mode, position=dropped intervals, layer=dropped events, phase=API errors,
start=profile-bookkeeping ticks, end=interval count, user_100ns=event count,
kernel_100ns=sampling stride, status=model ID, sampled=decoder mask.

## Device evidence and limitations

Basic events observed on the current Medium cache/runtime include QNN host time,
host RPC time, HTP RPC time, accelerator execution including and excluding
resource waits, and the configured HVX thread count. Detailed events additionally
include accelerator/operator cycles, operator timing, VTCM acquisition time, and
HVX/HMX power-on/acquisition time. Event units and parent relationships are
preserved; parent and child times must not be summed.

Important SDK behavior found during validation: alternating detailed-profiled and
unprofiled execution of the same graph caused DSP DMA error 6006 on a later
sample, with CPU fallback and a changed transcript. Retaining the profile handle
alone did not fix it. Continuous detailed profiling with sampled retrieval passed
the transcript gate. The profile handle now lives for the inference lifetime.
Detailed profiling substantially perturbs timing, so compare normal latency only
using uninstrumented/trace/basic repetitions with measured instrumentation cost.

Measured DDR bytes/bandwidth, compute occupancy, live accelerator clock rate, and
thermal throttling are not supplied by the events collected here. SDK graph
finalization bandwidth estimates are not measured runtime traffic. On this host,
thermal-zone WMI access is denied. Do not infer bandwidth saturation, utilization,
or throttling from latency or cycles alone. The old Task Manager percentage and
host-call duty remain distinct from useful accelerator execution.

## Medium findings

The completed second basic five-minute capture (`long/run-02-basic`) preserved
the established transcript SHA-256
`a609b84717a2e1699d94a53a353a25e1b3dd2151425552095d81208ee0632dd0`.
It recorded 277,996 intervals and 7,932 device events with no dropped records or
graph/API errors. Its 355.322 seconds wall time and 108.156 CPU-seconds describe
this session, not a speed comparison with the earlier 167-second single sample.

| Decoder graph family | Device samples | Median host call (us) | Median accelerator excluding waits (us) | Median paired difference (us) |
| --- | ---: | ---: | ---: | ---: |
| Self projection | 432 | 1114.35 | 308 | 806.70 |
| Self attention | 432 | 1080.35 | 289 | 787.85 |
| Fused cross/MLP | 432 | 2183.90 | 1363 | 817.40 |
| Logits | 22 | 5015.30 | 3937 | 1122.25 |

These are paired sampled durations, not a decomposition of total wall time.
The roughly 0.8 ms non-accelerator component in the three per-layer graph
families supports testing submission reduction and larger fused graphs. It does
not prove that all of that time is RPC, CPU execution, or removable overhead.
Accelerator resource-wait medians were about 57-75 us for these families.
Frontend/encoder/cross-KV each have only one sampled device execution per run at
the current stride, so their device figures describe the first window, not
steady-state averages.

The decoder made 53,352 calls to each of self projection, self attention, and
fused cross/MLP, plus 2,776 logits calls. All-call median/p99 host durations were
0.992/6.798 ms, 0.956/6.841 ms, 2.053/9.371 ms, and 4.853/13.336 ms respectively.
The per-layer submission count, not only the occasional slow call, is a major
optimization surface. No decoder offload exceeded 100 ms in this capture.

Process CPU accounting attributed 67.594 CPU-seconds to token selection out of
97.203 CPU-seconds inside decoder-token intervals and 108.156 for the whole
process. Selection therefore remains the first CPU optimization target: exact
suppression/sampling and worker dispatch, or a semantics-preserving NPU selection
path. This accounting does not replace stack sampling, and selection time is
already contained in token time.

The 2,848 decoder steps included 625 prefix-reused steps and an estimated 1,448
retry steps. Windows 1, 2, and 8 used all four attempts; other windows used one or
two. Retry work reuse remains relevant, but changing acceptance or sampling
policy is a quality change, not a transparent performance optimization.

The detailed short capture reports high cycle costs for cross-attention score
and value operations (pooled medians about 633,001 and 245,128 cycles) within the
fused cross/MLP graph. The SDK reports some fused nodes with zero cycles; zero
attributed cycles do not mean the underlying operation is free. Treat these as
operator-attribution leads, not independent node costs that can all be summed.

## Bit-conversion optimization (2026-09-15)

Replaced only the byte-copy loops in `math_double_bits` and `math_bits_double`
with C union representation conversions. There is no change to logarithm
arithmetic, sampling policy, or graph configuration. Under the existing Clang
`-Oz -ffreestanding -fno-builtin` flags, ARM64 `math_abs` now compiles to `fabs`
and `ret`, without the conversion loops.

Fresh release builds were compared with diagnostics disabled, Medium,
`fused,self,logits`, automatic worker selection, and identical staged QNN runtime
files and model cache. Three 30-second pairs ran in before/after, after/before,
before/after order; one five-minute pair ran after/before. Raw summaries,
transcripts, executable/runtime hashes, and power-plan inventory are preserved
in [`../data/bitcast-optimization-20260915/`](../data/bitcast-optimization-20260915/).

| Measurement | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| 30-second clip median elapsed, three runs each | 19.820 s | 19.704 s | 0.6% |
| 30-second clip median process CPU | 10.500 s | 10.094 s | 3.9% |
| Five-minute clip elapsed, one run each | 170.691 s | 157.347 s | 7.8% |
| Five-minute clip process CPU | 98.250 s | 51.469 s | 47.6% |
| Five-minute final-normalization/logit stage elapsed | 26.337 s | 7.933 s | 69.9% |

All eight runs exited successfully and matched transcript hashes, token counts,
decoder steps, prefix reuse, and decoder graph submission counts within each
fixture. The five-minute transcript retains SHA-256
`a609b84717a2e1699d94a53a353a25e1b3dd2151425552095d81208ee0632dd0`,
with 1,352 generated tokens, 2,848 steps, and 625 prefix-reused steps. The small
short-clip wall-time difference should not be treated as a robust speedup. The
long result is one paired observation, not a repeated performance estimate.

Five-minute total NPU host-call time was 126.732 s before and 131.222 s after.
The observed elapsed improvement therefore did not depend on faster graph calls;
host-call duty increased as CPU work decreased. This is not a hardware occupancy
measurement. Startup artifact hashing and graph submission overhead remain.

The existing math fixture now tests 14 explicit IEEE-754 edge patterns and
65,536 deterministic patterns, including signed zero, subnormals, infinities,
and signaling/quiet NaN payloads. The baseline and optimized ARM64 no-CRT tests
passed, as did the optimized x64 no-CRT test and full probe mock regression
suite. The optimized probe still imports only KERNEL32. Linux/macOS and `ncc`
self-host gates were not run in this Windows measurement session.

Preserved baseline: `build/bitcast-before/npu_probe.exe`, SHA-256
`c4065343defe026c614a2ab14e049f09af0c298775defdcfb3652700c134a5d9`.
Optimized candidate: `build/bitcast-after/npu_probe.exe`, SHA-256
`3946598ddb1832ac672bfa9c74341ea2c8eeb47654de7559438b915e05ec4703`.
The normal build executable was not replaced.

An initial mistakenly selected full-recording run was stopped and excluded.
Its partial inventory is retained separately in the `-aborted-full-audio`
directory. The corrected local harness `tests/tmp/benchmark-medium-bitcasts.ps1`
checks durations before inference: `bundestag-hearing-300s-30s-f32.wav` is 30 s,
`bundestag-hearing-5min-16k-mono-f32.wav` is 300 s, and the untrimmed
`bundestag-hearing-16k-mono-f32.wav` is 6,420 s. These are sequential warmed
measurements, not controlled cold-start measurements.

## CPU stacks and scheduling

The user captured an elevated WPR trace on 2026-09-15 after installing Windows
ADK. The saved `tests/tmp/medium-cpu.etl` spans 54.680 seconds and reports zero
lost events and buffers. Its Medium process is PID 16196; the accompanying
`tests/tmp/medium-wpr.csv` is a two-window, trace-only capture with 33,796
intervals, no drops, and no graph/API errors. This is a separate capture from
the five-minute comparisons above.

Xperf's scheduled process CPU total is 24.176690 seconds. The streaming analyzer
independently reconstructs 24.176707 seconds, within 17 microseconds, with no
context-switch chain mismatches. Thread 14844 (application entry point) accounts
for 12.239 seconds; eleven threads starting at `windows_thread_entry` account
for 11.095 seconds combined. Scheduled intervals include interrupt time and are
not identical to `GetProcessTimes` accounting. Native reported process elapsed
time was 30.522 seconds, not 30.522 CPU-seconds.

Native token intervals contain 15.875 CPU-seconds, including 13.078 CPU-seconds
in selection. Selection spans 5.066 seconds of wall time across 462 calls;
parallel workers explain why CPU time can exceed elapsed time. There are 480
decoder steps, including 214 prefix-reused steps. Do not add selection CPU to
token CPU or treat these aggregate comparisons as exact ETW/native alignment.

Xperf did not resolve the local PDB, but LLVM resolved sampled application RVAs
against the preserved symbol executable and its PDB. The raw export contains
23,161 process samples, of which 16,518 land in the application:

| Exclusive sampled function | Samples | Share of application samples |
| --- | ---: | ---: |
| `math_double_bits` | 4,787 | 28.98% |
| `math_bits_double` | 3,700 | 22.40% |
| `math_log` | 3,985 | 24.13% |
| `whisper_artifact_hash_update` | 2,929 | 17.73% |

The two bit-conversion helpers alone represent 51.38% of application samples
and 36.64% of all process samples. Their byte-copy loops in `src/shared/math.c`
are the strongest exact-semantics CPU optimization candidate. The logarithm
series is next; `sample_gumbel` calls two logarithms per vocabulary item when
filling noise values. Available caller chains are incomplete, so these samples
do not prove that every math-helper hit belongs to Gumbel generation. Artifact
hashing is another substantial cost; preserve integrity validation when testing
improvements. Sample shares are neither precise CPU durations nor predicted
end-to-end speedups.

Observed ready-to-running delays across process threads have median 16 us,
p95 3,728 us, and maximum 46,023 us. Main-thread observed ready delays sum to
4.471 seconds across 32,262 observations. These are scheduling delays, not
blocked NPU execution, and sums across parallel threads are not wall time.
Do not infer a scheduling fix or its benefit without a controlled comparison.

The reproducible local report is `tests/tmp/medium-etw-analysis.json`.
`tools/analyze-whisper-etw.py` streams Xperf dumper CSV, resolves application
addresses with `llvm-symbolizer`, and reports per-thread scheduling, observed
ready delays, exclusive samples, and sample-keyed inclusive stacks. Its
`--self-test` checks scheduling arithmetic, ready delays, and unrelated-stack
exclusion. SDK/kernel private symbols and complete caller attribution remain
unresolved. Hardware occupancy, measured DDR traffic, live accelerator clocks,
and thermal-throttling counters are still unavailable.

For a future kernel CPU/scheduling trace, run the following yourself in an
administrator PowerShell terminal from the repository root. Do not start it if
another WPR recording is active. `GeneralProfile` collects substantially more
than the application CSV and can contain system activity; review it before
sharing. Windows Performance Analyzer is available through the Windows ADK for
inspection. A symbol/unwind-enabled matching application build is needed for
reliable application stack attribution; release images are stripped. Build an
isolated symbol-enabled image with `tools/build.ps1 -DebugSymbols -BuildDir
experimental/snapdragon/build/diagnostics-symbols`, then stage the same runtime
files beside it. Keep the generated PDB beside its matching executable. This
retains optimization but adds unwind metadata and debug information, so measure
its overhead separately from the release image.

```powershell
wpr -status
wpr -start GeneralProfile -filemode
try {
    .\experimental\snapdragon\build\diagnostics-symbols\npu_probe.exe --diagnostics=trace --diagnostics-output=tests/tmp/medium-wpr.csv .\experimental\snapdragon\data\bundestag-hearing-5min-16k-mono-f32.wav
} finally {
    wpr -stop tests/tmp/medium-cpu.etl
}
```

Inspect CPU Usage (Sampled), CPU Usage (Precise), thread ready/wait intervals, and
module stacks alongside token selection and QNN host-call gaps. The collector
does not automatically start privileged recordings. Live DDR/clock/thermal counters
may additionally require Qualcomm-supported tooling or driver access; there is
no verified generic Windows counter replacement in this environment.

To reproduce the saved capture's address-based analysis without new inference:

```powershell
$xperf = 'C:/Program Files (x86)/Windows Kits/10/Windows Performance Toolkit/xperf.exe'
& $xperf -i tests/tmp/medium-cpu.etl -o tests/tmp/medium-etw-kernel.csv -a dumper
python experimental/snapdragon/tools/analyze-whisper-etw.py tests/tmp/medium-etw-kernel.csv --pid 16196 --image experimental/snapdragon/build/diagnostics-symbols/npu_probe.exe --base 0x7ff7c1640000 --size 0x415000 --output tests/tmp/medium-etw-analysis.json
```

The PID, load base, and image extent above belong only to this saved capture;
derive them from Xperf's `-a process -image detail` export for another recording.
Keep the exact captured executable/PDB rather than rebuilding over them. The
raw text export is about 1.1 GiB and contains system-wide activity; keep it local.