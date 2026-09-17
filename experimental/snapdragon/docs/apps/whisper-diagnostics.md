# Medium diagnostics

Local deployment was consolidated on 2026-09-15 after the benchmarks below.
The fused Medium runtime is now deployed; obsolete contexts and build variants
were removed. See [the retained runtime instructions](../README.md#retained-local-runtime-2026-09-15).
The historical `build/diagnostics-symbols` and `build/bitcast-symbols` executable/PDB
pairs are archived under `data/build-cleanup-20260915/` with the same directory
names. Use those archived paths for saved-trace symbolization; live capture
requires restaging the matching runtime and context or rebuilding a symbol image.

The collected five-minute comparisons and graph/CPU breakdowns are in
[`../data/medium-diagnostics/long/report.md`](../../data/medium-diagnostics/long/report.md).
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
in [`../data/bitcast-optimization-20260915/`](../../data/bitcast-optimization-20260915/).

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
long result above is the initial paired observation; the repeated follow-up
below supersedes it as the current performance estimate.

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

## Repeated bitcast follow-up (2026-09-15)

Two more alternating five-minute pairs and one optimized basic-profile run
completed with unchanged executable/runtime/audio identities, transcripts,
decoder work counts, and graph submissions. The original pair plus these two
pairs give three uninstrumented runs per variant. The profiled run is excluded
from performance statistics. Raw measurements and the complete graph breakdown
are in [`../data/bitcast-followup-20260915/report.md`](../../data/bitcast-followup-20260915/report.md),
with structured results in `comparison.json` alongside it.

| Five-minute pair | Before elapsed s | After elapsed s | Before CPU s | After CPU s |
| --- | ---: | ---: | ---: | ---: |
| 1 (after/before) | 170.691 | 157.347 | 98.250 | 51.469 |
| 2 (before/after) | 162.852 | 131.938 | 81.734 | 36.813 |
| 3 (after/before) | 163.070 | 148.164 | 88.109 | 45.453 |

Median elapsed time decreased **9.14%**, from 163.070 to 148.164 seconds.
Median process CPU decreased **48.41%**, from 88.109 to 45.453 CPU-seconds.
All three pairs improved, but elapsed reductions range from 7.82% to 18.98%:
this remains a small sequential, warmed sample with substantial runtime
variation. Median NPU host-call totals were almost unchanged, 126.732 versus
126.804 seconds. Do not attribute the unusually fast second optimized run
entirely to the bitcast change.

The optimized basic-profile run took 150.800 seconds and 49.031 CPU-seconds.
It recorded 277,996 intervals and 7,932 device events without drops, graph/API
errors, or thread-sampling errors. Bookkeeping measured 141.837 ms. Its elapsed
time is 1.78% above the optimized uninstrumented median, but one such comparison
does not isolate instrumentation overhead from run variation.

Token intervals account for 40.938 CPU-seconds, including 15.438 in selection
(31.5% of total process CPU). Token accounting includes 16.031 kernel CPU-seconds.
These are process counters and thread snapshots, not a new function-level CPU
profile. The old pre-bitcast ETL must not be used to rank current CPU hotspots.

| Decoder family | Calls | Host total s | Sampled host median us | Sampled accelerator excluding waits median us | Paired difference median us |
| --- | ---: | ---: | ---: | ---: | ---: |
| Self projection | 53,352 | 31.031 | 525.6 | 164.0 | 362.2 |
| Self attention | 53,352 | 27.030 | 480.7 | 110.0 | 368.9 |
| Fused cross/MLP | 53,352 | 57.891 | 992.0 | 608.0 | 368.1 |
| Logits | 2,776 | 7.973 | 2728.0 | 2160.0 | 586.3 |

Per-layer families each have 432 device samples; logits has 22. Paired differences
are not simply RPC costs or guaranteed removable time. Host-call duration still
does not measure hardware occupancy, DDR bandwidth, or thermal throttling.

**Next experiment:** prototype a single-layer fused self-projection/attention
graph with a supported on-NPU KV-cache update. The current
`whisper_decoder_qnn_self_attention_offload` executes projection, updates the
shared key/value caches on the CPU, then executes attention. A successful fusion
could eliminate one submission per executed layer (53,352 submissions in this
fixture), but cannot be implemented as a simple graph concatenation. First
validate cache-update operator support, exact causal/cache behavior, FP16 output
equivalence, and memory placement on one layer; only then regenerate and measure
the full Medium context. Retain the existing cache and implementation until those
checks pass. No graph or runtime changes were made during this follow-up.

No fresh privileged WPR recording was made. A separate optimized symbol build
and matching runtime are staged under `build/bitcast-symbols`; the preserved
pre-optimization symbol image is unchanged. For updated function-level CPU
sampling, run the following from an administrator PowerShell after checking that
no other WPR recording is active:

```powershell
wpr -status
wpr -start GeneralProfile -filemode
try {
  .\experimental\snapdragon\build\bitcast-symbols\npu_probe.exe --diagnostics=trace --diagnostics-output=tests/tmp/medium-bitcast-wpr.csv .\experimental\snapdragon\data\bundestag-hearing-5min-16k-mono-f32.wav
} finally {
  wpr -stop tests/tmp/medium-bitcast-cpu.etl
}
```

Keep this system-wide trace local. The normal executable remains unchanged;
the optimized release candidate is still `build/bitcast-after/npu_probe.exe`.

## One-layer self fusion (2026-09-15)

The isolated [`self_fusion_probe.c`](../../src/apps/whisper/benchmarks/self_fusion_probe.c)
now combines Medium layer-zero self projection, KV insertion, and attention into
one QNN graph. It reuses the existing private builder and actual trained FP16
weights via textual inclusion; production graph code and context caches are
unchanged. The probe loads and validates the complete self-attention artifact
before restricting graph construction to one layer. Its tensor-index mappings
must track the private builder; tensor-count assertions detect count changes,
not reorderings.

On Snapdragon X Elite with QAIRT 2.50.0.260828 (QNN core 2.39, HTP 5.50), dynamic
INT32 `ScatterElements` indices work for both FP16 cache layouts: keys
`[16,64,448]` on axis 2 and values `[16,448,64]` on axis 1. Two separate capability
tests check boundaries, repeated insertion, and every untouched cache value.
The fused graph keeps projected Q/K/V native and inserts K/V on the NPU before
attention, removing one host submission and the CPU KV-copy loop per layer.
The CPU still prepares indices and the causal mask.

The split baseline uses registered FastRPC caches, as production does. The fused
path uses distinct registered input/output cache banks, swapping them after each
step; it does not assume in-place tensor aliasing. Each run checks **1,352 cases**:
three complete 448-position sweeps, then positions 0, 1, 2, 7, 127, 447, 0, 1.
Caches remain populated between sweeps, exercising masked stale future entries
and overwrites. Every key/value element and every final attention-output FP16 bit
matches the split reference. This is split/fused equivalence on deterministic
inputs, not an independent attention oracle or transcript-quality test.

Execution order alternates each step. QPC intervals include either projection,
CPU cache insertion, and attention, or the single fused execution. Tensor binding,
input/mask/index preparation, and full-cache comparisons are outside the intervals.
The first 16 positions of each sweep are excluded, leaving 432 samples per row.
Reported microseconds are truncated integers; median uses the two middle samples.

| Build/run | Sweep | Split median us | Fused median us | Split mean us | Fused mean us |
| --- | ---: | ---: | ---: | ---: | ---: |
| Initial shared-cache harness | 1 | 725 | 522 | 930 | 632 |
| Initial shared-cache harness | 2 | 692 | 506 | 892 | 592 |
| Initial shared-cache harness | 3 | 709 | 510 | 877 | 635 |
| Reproducible release build | 1 | 677 | 489 | 899 | 624 |
| Reproducible release build | 2 | 587 | 425 | 689 | 507 |
| Reproducible release build | 3 | 629 | 479 | 872 | 603 |
| Final guards/cleanup build | 1 | 531 | 398 | 675 | 525 |
| Final guards/cleanup build | 2 | 530 | 391 | 719 | 504 |
| Final guards/cleanup build | 3 | 525 | 384 | 671 | 480 |

All three runs passed correctness and memory/context/device/backend teardown.
The final build's median of sweep medians is 530 -> 391 us (**26.2% lower**).
Within-sweep reductions span roughly 24-29% across the runs. Absolute latency
varies substantially, and the first harness uses a different linker flag set;
do not pool these as nine independent production measurements. Full logs,
including p95 and maximum latency, are retained under
`../data/self-fusion-20260915/`. The final executable imports only KERNEL32;
QNN/FastRPC remain dynamically loaded vendor dependencies.

Reproduce from the repository root on Windows ARM64 with Clang and the existing
exported Medium artifacts. Stage a matched QAIRT runtime, including DSP SO/CAT
files, beside the probe. For this workspace the validated runtime is in
`build/bitcast-after`:

```powershell
& ./experimental/snapdragon/tools/whisper/build.ps1 -SelfFusionProbe -BuildDir experimental/snapdragon/build/self-fusion-probe
Get-ChildItem experimental/snapdragon/build/bitcast-after -File |
  Where-Object { $_.Extension -in @('.dll', '.so', '.cat') } |
  Copy-Item -Destination experimental/snapdragon/build/self-fusion-probe
& ./experimental/snapdragon/build/self-fusion-probe/self-fusion-probe.exe
if ($LASTEXITCODE -ne 0) { throw 'Self-fusion validation failed' }
```

Do not run concurrent inference during measurement. The probe rebuilds only its
isolated graphs in memory, never serializes or overwrites a production context,
and fails on any graph error or bit mismatch. Normal build behavior is unchanged
when `-SelfFusionProbe` is absent.

**Decision:** proceed to an opt-in full-Medium candidate, retaining the split
implementation and original context. One KV bank is 1.75 MiB per layer; ping-pong
adds **42 MiB** across 24 layers and introduces full-cache read/write traffic.
The one-layer harness allocates 5.25 MiB for baseline plus two fused banks.
Next gates are full-context build/restore and memory placement, retry/reset and
prefix-reuse correctness, unchanged transcript/work counts, then alternating
five-minute wall-time and process-CPU measurements. No full-model context was
regenerated or deployed here. CPU savings, hardware occupancy, device execution
time, DDR traffic, and end-to-end speedup are not established by this probe.

## Full-Medium self-fusion candidate (2026-09-15)

The one-layer experiment above has now been integrated into an **opt-in** Medium
candidate. `tools/whisper/build.ps1 -SelfFusionCandidate` defines `WHISPER_SELF_FUSION=1`
and defaults to `build/self-fusion-candidate`, leaving the normal executable
untouched. Other models and Medium modes without `self` retain their split graph
behavior. The original split implementation remains available in normal builds.

Each Medium self layer now executes projection, native Q/K/V, two cache insertion
nodes, and attention in one graph. Each layer independently tracks its current
registered cache bank, advancing only after successful execution. Skipped prefix
steps do not advance banks; repeated positions and new windows overwrite the
current position while future cache entries remain causally masked. The self
offload callback returns the number of graph submissions (zero on unavailable,
one fused, two split), keeping diagnostic work counts accurate.

The candidate has a distinct context filename,
`whisper-medium-m1c-self-fused-v1-encoder-fp16-l24.qnnctx`. Its decoder graph list
contains 50 graphs instead of 74; encoder graphs still restore into a separate
context through the existing placement path. The candidate metadata reuses the
otherwise unused projection/query ID slots for old K/V and position inputs;
therefore the candidate and split caches must never be renamed interchangeably.
The original context hash is checked before and after measurement.

The generated candidate context is 1,561,096,016 bytes, versus 1,562,037,952 for
the split context. The application shared cache grows by 42 MiB for the second
self KV bank. Context size and peak process memory are different measurements;
runtime allocation and placement can offset that extra application allocation.
The full context has passed build, selective restore, and frontend/encoder
execution. The integrated one-layer harness additionally passed 1,352 bit-exact
KV/output cases through restored tensor IDs and the production callback.

Reproduce the already-prepared benchmark from the repository root:

```powershell
./experimental/snapdragon/tools/whisper/benchmark-whisper-self-fusion.ps1 -ValidateOnly
./experimental/snapdragon/tools/whisper/benchmark-whisper-self-fusion.ps1 -OutputDirectory experimental/snapdragon/data/self-fusion-medium-repeat
```

Preparation uses `tools/whisper/build.ps1 -BuildDir experimental/snapdragon/build/self-fusion-baseline`
and `tools/whisper/build.ps1 -SelfFusionCandidate`, with matching QAIRT DLL/SO/CAT files
beside each binary. Build the separate candidate context once using
`build/self-fusion-candidate/npu_probe_builder.exe --model=medium --decoder-offload=fused,self,logits`.
This builder invocation runs capability probes before full-context generation;
none of that preparation time belongs in the inference comparison.

The runner performs a 35.008-second split/fused correctness pair, then three
five-minute pairs ordered split/fused, fused/split, split/fused. Diagnostics are
off. It rejects changed transcripts, work counts, executable hashes, runtime
hash mismatches, graph errors, CPU self fallback, and incorrect self submission
counts. Process wall time includes loading, restore, transcription, and cleanup.
Use `-ReportOnly` to regenerate the report without inference. Raw results,
inventory, and the paired report are under
[`../data/self-fusion-medium-20260915/`](../../data/self-fusion-medium-20260915/).

### Measured results

All eight runs passed, including all six five-minute runs. Each long run has
12 windows, 1,352 generated tokens, 2,848 decoder steps and 625 prefix-reused
steps. The transcript SHA-256 remains
`a609b84717a2e1699d94a53a353a25e1b3dd2151425552095d81208ee0632dd0`.
Self submissions fall from 106,704 to 53,352; fused cross/MLP submissions remain
53,352 and logits submissions remain 2,776. No CPU self fallback or graph
execution errors were accepted.

| Five-minute pair | Split wall s | Fused wall s | Split CPU s | Fused CPU s |
| --- | ---: | ---: | ---: | ---: |
| 1 (split/fused) | 149.091 | 127.170 | 49.672 | 39.922 |
| 2 (fused/split) | 147.120 | 126.753 | 48.313 | 40.000 |
| 3 (split/fused) | 150.075 | 127.428 | 49.391 | 41.438 |
| Median | 149.091 | 127.170 | 49.391 | 40.000 |

Median wall time is **14.70% lower**, and median process CPU is **19.01% lower**.
Paired wall reductions range from 13.84% to 15.09%; paired CPU reductions range
from 16.10% to 19.63%. Median self graphExecute host time falls from 58.102 s
to 38.556 s (33.64%). Context restore also improves, from 3.099 s to 1.566 s;
the end-to-end improvement should not all be attributed to steady-state decoder
execution. The 35.008-second gate matched 132 tokens and 214 reused steps, with
28.159 -> 22.554 s wall and 14.828 -> 11.734 CPU-seconds.

Median peak resident memory is 3,688,652,800 -> 3,680,378,880 bytes, and median
peak private commitment is 1,457,618,944 -> 1,451,384,832 bytes. Thus these runs
show no net process-memory increase despite the additional 42 MiB application
cache. This does not prove that the additional cache is free: the total includes
QNN allocation and context-placement effects, and no DDR traffic was measured.

The matched split executable SHA-256 is
`2d7457e01aa1c773ae423cec72d56ca1537f2332c0472e26ed24bbc1a61c1d5a`;
the fused candidate is
`e8dca312c36e671d665a329b2cf019f34010ad7daa3b7c536d2190622879f9f2`.
The fused context SHA-256 is
`38718e18021aa963677e593527f9b14e3f0f0a63eecfcc8145880db684b20979`.
All binary, runtime and context hashes were rechecked after measurement.
The baseline mock suite, integrated one-layer hardware test, full candidate
build/restore, and no-CRT import checks passed. Build logs and raw inference
output are retained beside the [generated report](../../data/self-fusion-medium-20260915/report.md).

**Decision:** the candidate improves both requested performance metrics on this
fixture and remains available for opt-in use. Normal deployment and the original
context are unchanged. These are three sequential warmed pairs on one recording,
not broad transcription-quality validation, cold-start statistics, or proof of
higher hardware NPU occupancy. Validate additional audio and candidate
cancellation/failure handling before promoting it to the default deployment.

## CPU stacks and scheduling (pre-bitcast capture)

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
`tools/whisper/analyze-whisper-etw.py` streams Xperf dumper CSV, resolves application
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
isolated symbol-enabled image with `tools/whisper/build.ps1 -DebugSymbols -BuildDir
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
python experimental/snapdragon/tools/whisper/analyze-whisper-etw.py tests/tmp/medium-etw-kernel.csv --pid 16196 --image experimental/snapdragon/build/diagnostics-symbols/npu_probe.exe --base 0x7ff7c1640000 --size 0x415000 --output tests/tmp/medium-etw-analysis.json
```

The PID, load base, and image extent above belong only to this saved capture;
derive them from Xperf's `-a process -image detail` export for another recording.
Keep the exact captured executable/PDB rather than rebuilding over them. The
raw text export is about 1.1 GiB and contains system-wide activity; keep it local.