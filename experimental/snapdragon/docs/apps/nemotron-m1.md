# Nemotron M1: Windows streaming feasibility

The approved Windows-only signed FastRPC FP32 executor now passes the full
M1 numerical gate against frozen M0 tensors. **M1 is provisionally accepted
by the user on 2026-10-14 with a latency waiver for the validated v17
snapshot.** Its three clean full-sequence p95s are 323.756 / 328.518 /
326.650 ms; the original strict 320-ms goal remains unmet.
Historical QNN failures are preserved below.
Neither a successful build nor a CPU diagnostic substitutes for DSP execution;
timing-only results never substitute for numerical acceptance.

The approval record is
`D:\newos-data\nemotron-asr\m1\provisional-acceptance-v17.json`.
Use its pinned `fastrpc-fp32-pad-v17\manifest.json` and matching immutable
host/DSP artifacts, not a newer experimental snapshot. The original gate
report and automated runner remain unchanged and still report
`m1_correctness_passed_performance_pending`; this waiver is an explicit
project decision, not a reclassification of a failing performance test.
Further optimization is paused. No deployed recognizer, live microphone
support, QNN graph acceptance or HMX execution is claimed.

## Implemented path

- [Direct graph exporter](../../tools/nemotron/nemotron_m1.py): pinned real
  checkpoint weights, separate first/steady subsampling, all 24 FastConformer
  layers, explicit attention/convolution caches, German acoustic conditioning,
  prediction and joiner probes. There is no ONNX converter/runtime requirement.
- [Freestanding probe](../../src/apps/nemotron): separate preparation and
  restoration executables using the existing QNN ABI. Generic portable
  readers validate envelopes, tensor dimensions, offsets, references,
  checksums, compatibility and carry lifetimes. A thin Windows adapter owns
  mappings, Unicode paths, DLL loading, QPC and process inventory.
- [Explicit build](../../tools/nemotron/build.ps1): ARM64, no CRT/libc,
  Kernel32-only static imports, real stack probing and isolated build/clean.
  The default all-app build and production distributions are unchanged.
- [Runner and evidence audit](../../tools/nemotron/nemotron_m1_run.py):
  preserves native exit codes/logs, verifies result hashes, reports numerical
  failures separately from measured timing, and refuses successful M1
  acceptance without the full carried-cache coverage.

  ### Approved Windows-only FP32 route

  On 2026-10-14, a project-owned FP32 HTP package was implemented and signed
  with the existing development identity. The pinned Windows SDK cannot
  prepare it: ARM64 CPU-target registration returns 4005 (`os_dlopen`
  unimplemented), null/default-target preparation fails, and emulated x64
  and legacy MSVC-ABI probes also fail before the package entry point runs.
  The signed build alone is not evidence of DSP inference.

  The user explicitly rejected WSL and Linux preparation hosts and approved
  using the existing signed OEM FastRPC transport for a project-owned FP32
  Hexagon/HVX graph executor instead. This intentionally changes Nemotron's
  M1 backend; it does not change M0, numerical tolerances or timing targets.
  New runs must identify the FastRPC backend, actual DSP execution and
  driver/module/catalog hashes. They must not claim QNN context or HMX
  acceptance. The other Snapdragon applications and default builds remain
  unchanged.

  [The FP32 graph exporter](../../tools/nemotron/nemotron_fp32.py) encodes
  operator shapes and parameters in immutable metadata. The
  [project-owned kernels](../../src/apps/nemotron/fp32/nm_fp32_kernels.c)
  implement float32 operators without libc or libm. Dense V73 HVX kernels
  reuse each weight tile across four audio frames; no half-precision tensor
  conversion is part of this path. The
  [Windows runner](../../tools/nemotron/nemotron_fastrpc.py) binds package and
  driver identities and requires independent numerical audits plus separate
  timing runs before emitting `m1_passed`.

  Development-only C tests pass for the operator set, cancellation-sensitive
  Add/MatMul cases, slicing and nonfinite-input rejection. The same C code
  also passed all 24 encoder layers, conditioning and continued caches for
  16 real M0 chunks, including speech onset and cache saturation. Those CPU
  diagnostics validate the algorithms only; signed DSP execution and the
  full 65-chunk hardware gate are separate requirements.

  The subsequent signed DSP runs passed those hardware numerical requirements:

  | Case | Consecutive steps | Independently compared elements | Violations |
  | --- | --- | --- | --- |
  | Long, including cache saturation and EOF | 65 | 194,396,995 | 0 |
  | Incomplete tail with state carry | 3 | 8,972,169 | 0 |
  | Silence and fresh reset | 4 | 11,962,892 | 0 |
  | Prediction, carried recurrent states | 64 | 204,800 | 0 |
  | Joiner, real token decisions | 64 | 837,632 | 0 |

  The Add, scalar MatMul and HVX MatMul cancellation smokes also passed on the
  signed DSP module. The host executable statically imports only Kernel32 and
  does not link model arithmetic; the QNN/QHPI-free DSP image executes it.
  Artifacts are under `D:\newos-data\nemotron-asr\m1\fastrpc-fp32-package`,
  with raw model graphs/cases and independent audits in adjacent directories.
  No WSL, Linux host, driver update, boot change or trust change was used.

  The initial precise long run measured roughly 7.5 seconds per chunk and
  therefore **does not pass** the 320-ms timing goal. A matched-package dense
  microbenchmark isolates 16 MiB of coefficients rather than the full 2.24-GiB
  model. Timing investigation must separate model validation/cache maintenance,
  RPC transfer and DSP arithmetic before claiming a fast precise backend.
  Host and DSP executable hashes are pinned together to prevent a concurrent
  build's protocol from being paired with an older signed module.

  Warm-model optimization uses four DSP workers, exact column-tiled FP32
  coefficients, bulk layout copies and vector arithmetic. Packed MatMul
  metadata uses version2 and `weight_tiles=32`; logical shapes and coefficient
  values remain unchanged. Pointwise convolution acceleration reduced the
  packed-tail warm diagnostic to approximately 674-677 ms.

  Signed snapshot `fastrpc-fp32-prefetch-v14` additionally prefetches bounded
  128-row windows of immutable packed coefficients into DSP L2, retaining the
  same FP32 multiply and four-partial reduction order. Counter-off matched
  runs reduced the dense microbenchmark's pure kernel median from 2.540 to
  1.301 ms and the 315 encoder MatMul calls from 488-490 to 267-269 ms.
  Independent audits retained zero violations across 131,072 microbenchmark
  elements and 8,972,169 carried-tail elements. Shared-memory cache flags and
  production compiler flags, including `fp-contract=on`, remained unchanged.

  The three-step tail's last warm model-inclusive sample fell from 687.838 to
  475.946 ms; its two post-initialization chunks measured 475.946-477.446 ms.
  These short diagnostics establish an improvement, not final p95 acceptance:
  the full 65/3/4 numerical coverage and three clean repeated benchmark runs
  must be rerun with the final candidate. The unchanged 320-ms goal still
  fails. Evidence is in
  `D:\newos-data\nemotron-asr\m1\fastrpc-fp32-perf\prefetch-ab-report.json`.

  Signed snapshot `fastrpc-fp32-aligned-v15` combines checked 128-byte
  coefficient staging with guarded aligned HVX loads; unaligned inputs retain
  their existing path. Actual DSP pointers confirmed all 243 packed encoder
  RHS tensors and all 726 static FP32 encoder tensors were aligned. Staging
  added 239,232 bytes across the three graphs, preserved the 512-MiB segment
  cap and left source graph hashes and coefficient values unchanged.
  Independent micro/tail audits and original precision smokes passed.
  Matched warmed timing-only controls measured model-inclusive p95 of
  461.004 ms for v14, 461.363 ms for v15 with raw staging, and 395.515 ms for
  aligned v15. These remain short diagnostic controls, not final acceptance.
  Evidence is in
  `D:\newos-data\nemotron-asr\m1\fastrpc-fp32-perf\alignment-staging-ab-report.json`.

  Signed snapshot `fastrpc-fp32-partial-v16` vectorizes irregular-width
  attention MatMul, including 60-column outputs, with bounded final-K-row
  loads and partial output stores. Existing full-width, packed and
  bias-bearing paths remain supported. Independent audits passed 2,700
  irregular-shape elements, 37 original precision-smoke elements, 131,072
  dense microbenchmark elements and 8,972,169 carried-tail elements with zero
  violations. Matched warmed model-inclusive diagnostics improved from
  399.933 to 338.369 ms. Separate profiling measured 315 MatMul calls at
  137.864 ms pure execution and no remaining scalar MatMul work.
  This is still above 320 ms, without full65 acceptance. Evidence is in
  `D:\newos-data\nemotron-asr\m1\fastrpc-fp32-perf\partial-v16\partial-column-ab-report.json`.

  Signed snapshot `fastrpc-fp32-pad-v17` replaces per-element Pad coordinate
  calculations with bounded row-span copies and HVX constant fills. Hardware
  padding regressions passed all 13,656 outputs bit-for-bit, including 3,293
  negative zeros. Full 65/3/4 numerical audits and the 64-step prediction and
  joiner audits passed with zero violations.

  Three clean full-sequence model-inclusive warm p95 measurements were
  **323.756 / 328.518 / 326.650 ms**. All exceed 320 ms, so v17 still has status
  `m1_correctness_passed_performance_pending`. Short diagnostics do not
  override this full gate. Evidence is in
  `D:\newos-data\nemotron-asr\m1\fastrpc-fp32-pad-v17-gates\report.json` and
  `D:\newos-data\nemotron-asr\m1\fastrpc-fp32-perf\pad-v17\pad-span-gates-summary.json`.

  `GetProcessTimes` measured warm host user+kernel CPU totals of
  1.078 / 1.359 / 1.141 seconds per measured 65-step pass, excluding setup and
  three warmups. These are process CPU measurements, not estimates from
  wall time minus DSP time, and do not include a live audio frontend.
  Actual placement remains HVX plus DSP scalar, without HMX, GPU or host
  model inference.

  The v18 vector Sigmoid experiment was rejected: although it met M0's
  unchanged tolerances, 162 of 10,098 outputs exceeded the additional 2e-7
  absolute-error limit against stable float64 sigmoid. Maximum error was
  3.326393044e-7. No broader validation or performance claim followed.
  The working kernel was restored to v17 and verified against its source
  hash; the expanded 13 development test suites are retained.
  Future optimization can revisit the approximately 32-ms Sigmoid and
  approximately 54-ms steady front Conv hotspots without relaxing numerical
  requirements.
  Final timing acceptance includes subsampling as well as the encoder and
  cache copies, not just the encoder's core loop.

The historical QNN probes execute model operations through `QnnHtp.dll`;
the approved FastRPC path executes them in the signed project-owned DSP
module. Host work is scheduling, checked I/O/buffer copies and output
comparisons; neither path has a CPU model fallback or external inference
framework in its PE.

## Streaming graph contract

The encoder is deliberately split into three finalized contexts:

1. `nemotron_subsampling_first`: 25 mel frames, initial causal padding.
2. `nemotron_subsampling_steady`: 32 mel frames, carried subsampling caches.
3. `nemotron_encoder`: four subsampled frames, 56 fixed attention-history slots,
   24 causal convolution caches, and the acoustic language/projector branch.

Each chunk executes first **or** steady subsampling, then the encoder. The
subsampling result feeds the encoder in the same step; model states feed the
next step. Buffers remain distinct and carries are copies, never I/O aliases.
Leading unused attention slots are zero and masked. Logical valid history
grows by four until saturation at 56, matching the pinned chunked-limited
`[56,3]` profile. No future frames are silently batched.

Subsampling cache storage is NHWC, including the frequency-padding columns;
conformer convolution history is time-major. M0's cache arrays are packed
into these defined layouts by the developer fixture tool, not substituted
for live native output. Actual previous outputs are carried during execution.

Relative-position projections for the fixed distance range, and the fixed
German one-hot contribution, are constant-folded offline. These are immutable
model expressions independent of audio, not hidden host inference operations.
Both the raw and language-conditioned/projected encoder results are checked.

The FP32 CPU interpreter validated the fixed-cache adapter against all M0
encoder outputs and state tensors for 65 consecutive chunks, including history
saturation and EOF. Maximum reported raw encoder difference was
`5.662441253662109e-7`. This establishes adapter semantics, not HTP parity.

## Artifact envelope

[The format constants](../../src/apps/nemotron/nemotron_format.h) and Python
writer describe the same little-endian, versioned envelope:

| Kind | Payload |
| --- | --- |
| `.nmg` graph | Named tensor contracts, QNN nodes/parameters and aligned immutable weights |
| `.nmc` context | Actual registered app-I/O IDs/contracts and opaque finalized QNN binary |
| `.nmf` fixture | Flat graph-I/O table, per-step data/zero/carry cells and FP32 expected outputs |
| `.nmr` result | Per-step QPC/error metrics and actual output tensors |

The 256-byte header binds model fingerprint, accepted M0 contract hash,
runtime DLL hash, core ABI, SDK metadata, V73, German prompt9, 320-ms profile,
precision and payload SHA-256. Tables and raw spans are bounded and validated;
unknown versions, truncation, overflows, invalid aliases, unavailable/stale
carry sources and allocation failures are errors.

The header's backend metadata `2/18` is the **SDK YAML common backend API
tag**, not the provider implementation version. The actual pinned HTP provider
reports **5.50.0** alongside core **2.39.0**; that version and loaded DLL paths
are printed by the native audit. Runtime compatibility is additionally bound
to the pinned DLL's SHA-256.

Compared with M0's individual tensor files, fixture/result tensors are packed
into a small number of indexed binary envelopes. The earlier M0 evidence is
not modified.

## Reproduction

Run from the repository root, checking each command's exit code. Choose fresh
output directories: preparation, fixture export and native execution do not
overwrite earlier evidence.

```powershell
$Python = '.\experimental\snapdragon\build\gemma-oracle-x64\python.exe'
$Tools = '.\experimental\snapdragon\tools\nemotron'
$Data = 'D:\newos-data\nemotron-asr'
$Deps = "$Data\reference-packages"
$M1 = "$Data\m1\reproduction"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$Tools\build.ps1" -Test
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$Tools\build.ps1" -TestLifecycle

& $Python -B "$Tools\nemotron_m1.py" --data-dir $Data --deps-dir $Deps `
    cpu-check --fixture long --steps 65 --full --output "$Data\m1\cpu-reproduction.json"
& $Python -B "$Tools\nemotron_m1.py" --data-dir $Data --deps-dir $Deps `
    export --output "$M1\risk-graphs"
& $Python -B "$Tools\nemotron_m1.py" --data-dir $Data --deps-dir $Deps `
    cases --graphs "$M1\risk-graphs" --output "$M1\risk-cases" `
    --fixtures long --max-steps 2 --risk
& $Python -B "$Tools\nemotron_m1.py" --data-dir $Data --deps-dir $Deps `
    decoder-cases --graphs "$M1\risk-graphs" --output "$M1\decoder-cases" --max-steps 64
python -B "$Tools\nemotron_m1_run.py" --data-dir $Data run `
    --graphs-dir "$M1\risk-graphs" --fixtures-dir "$M1\risk-cases" `
    --decoder-fixtures-dir "$M1\decoder-cases" --output "$M1\risk-run"

# This is a correctness experiment, not an accepted production build.
& $Python -B "$Tools\nemotron_m1.py" --data-dir $Data --deps-dir $Deps `
    export --graphs subsampling-first,subsampling-steady,encoder --output "$M1\full-graphs"
& $Python -B "$Tools\nemotron_m1.py" --data-dir $Data --deps-dir $Deps `
    cases --graphs "$M1\full-graphs" --output "$M1\full-cases"
python -B "$Tools\nemotron_m1_run.py" --data-dir $Data run --full `
    --graphs-dir "$M1\full-graphs" --fixtures-dir "$M1\full-cases" --output "$M1\full-run"
```

The current native numerical experiment is expected to return a nonzero
correctness status. Preserve the resulting report and logs. Do not hide that
status in a wrapper or change the tolerance merely to make the command green.

The developer-only `--precision 32`, `--fused-prediction`, `--fully-connected`
and `--centered-norm` switches reproduce the bounded trials below. They are
not successful alternative reference profiles.

Direct native interfaces:

```text
nemotron-prepare.exe prepare graph.nmg new-context.nmc
nemotron-probe.exe validate artifact
nemotron-probe.exe run context.nmc case.nmf new-result.nmr
nemotron-probe.exe pipeline first.nmc steady.nmc encoder.nmc case.nmf new-result.nmr
```

## Measured gate results

The complete 24-layer FP16 graph finalized on actual HTP; its opaque context
is **1,188,200,944 bytes**. Native execution covered the real 65-chunk long
case plus incomplete-tail and silence cases with continued caches.
**Numerical encoder/cache comparisons fail.**

The corrected long run measured warm p95 encoder execution plus cache copies
at **32.609 ms**, below the 320-ms intermediate target. Measured p95 wall time
was **97.205 ms**, including validation/comparison overhead. The timing target
is separate from, and cannot substitute for, numerical correctness.
These are the original verification-run statistics, not clean inference-only
benchmarks; the later measurements below separate the test harness overhead.

Early probes:

| Variant | Result |
| --- | --- |
| Joiner FP16, 64 real decisions | 837,632 logits compared; zero tolerance violations |
| Prediction FP16, 64 carried token transitions | 204,800 elements compared; six violations |
| Prediction fused gates FP16 | Nine violations; rejected |
| Prediction FP32 interfaces | Six violations; rejected |
| Encoder risk FP16, two carried chunks | 2,049 violations |
| Encoder risk FP32 interfaces | 1,982 violations; rejected |
| Encoder risk FullyConnected rewrite | 2,049 violations; no improvement |
| Encoder risk centered-variance rewrite | 44,141 violations; rejected |

FP32 input/output declarations do not demonstrate FP32 arithmetic inside HTP.
No trial passed by widening the frozen absolute/relative M0 budgets.

Two preparation bugs were found and corrected before final gate reporting:

- QNN depthwise filters require `[H,W,1,channel_out]`, not `[H,W,channel_out,1]`.
  The invalid prototype caused a backend finalize divide-by-zero. Export now
  follows the SDK definition, and native preflight rejects the malformed shape
  before QNN initialization.
- Upstream subsampling caches mutate in place. Expected fixture snapshots
  must be cloned immediately, not retained as views until the end of a run.
  Final pipeline validation uses those immutable snapshots.

The actual loaded FastRPC DLL is the `qcnspmcdm8380.inf_arm64_553f7b0d71497f2c`
driver-store copy. Pure restore/execution does not load `QnnHtpPrepare.dll`.
Vendor CRT imports remain the documented Qualcomm runtime exception; the
project PEs statically import only Kernel32.

QPC measures cold restore, graph execution, input/cache copies and complete
chunk processing. BASIC QNN cycle events were unavailable; `profileCycles=0`
does **not** mean zero NPU work. Operator-level HMX/spill/fill attribution
remains unmeasured and is not claimed.

## Tests and next gate

The original native suite passed **77 reader checks**, preserving the original65,
plus actual FP16/FP32 HTP lifecycle, three-context carry, intentional
comparison-failure and unsafe-clean rejection tests. The Python format suite
tests binary roundtrips, dtype/shape/carry rules, depthwise layout, CPU
normalization semantics and result checksum rejection.

### 2026-10-14 investigation

Per-operator capture and actual-input shadow replay are now implemented in
[nemotron_trace.py](../../tools/nemotron/nemotron_trace.py). Captures promote
intermediate tensors to app-readable outputs, compare against the FP32
interpreter, and optionally evaluate each operator using its actual preceding
HTP output. This distinguishes propagated error from a local operator error.
These **teacher-forced diagnostic traces are not acceptance fixtures**.
The final pipeline tests continue to carry actual native caches.

The first float subsampling violation appears in the stem convolution
(maximum absolute difference about `0.1064`). In the encoder, the first
feed-forward down-projection amplifies rounding/cancellation differences
(about `2.3169` maximum absolute difference in the traced first chunk).
Relative-position attention, convolution and mutable caches propagate them.
The SDK explicitly documents that V73 HTP runs float32 declarations using
float16 math; changing I/O dtype does not supply float32 internal arithmetic.
Enabling the pinned SDK's precision-compensation and high-precision-sigmoid
options did not fix the captured float results.

[nemotron_quant.py](../../tools/nemotron/nemotron_quant.py) implements additional
explicit precision experiments: real speech/silence/cache calibration,
16-bit fixed-point activations and weights, 32-bit convolution biases,
per-channel signed weights, and explicit float normalization between
Dequantize/Quantize nodes. A Boolean attention mask avoids consuming the
score quantizer's dynamic range with the old `-10000` sentinel. Input range
overflow and invalid carries remain errors. These are experimental candidates,
**not a validated replacement reference or successful M1 profile**.

Those trials also fail the unchanged M0 gate. An independent Python auditor
recomputed every dequantized output for the 65-chunk per-channel full run:
**117,766,878 violations over 194,396,995 compared elements**, matching the
native counters. The earlier FP16 run had 148,090,243 violations. Improvement
is not acceptance; none of these runs permits M2 to proceed as passed.

The native reader now supports backward-compatible version2 scalar and
per-axis signed quantization descriptors. It checks scale tables, axis/count,
offsets, span nonoverlap and finite positive scales; carry buffers must have
identical encodings. Quantized zero initialization uses the correct encoded
zero rather than zero bytes. Version1 float artifacts remain supported.
**231 native checks** and **11 Python format/audit tests** passed, including
all prior checks and timing-only negative acceptance tests.

### Clean execution-time measurements

The separate `run-benchmark` / `pipeline-benchmark` commands execute three
complete warmup passes before measuring. The measured loop keeps actual
state carry, binding/cache copies, input validation and HTP graph execution,
but removes output comparisons, output-result snapshots and repeated profile
creation. Result cells are ZERO and compared-element counts are zero:
correctness is explicitly **not assessed** by a timing-only result.

Three independent FP16 pipeline processes measured:

| Run | Warm p95 encoder + copies | Warm p95 complete graph/I/O loop |
| --- | --- | --- |
| 1 | 28.900 ms | 29.711 ms |
| 2 | 43.001 ms | 43.872 ms |
| 3 | 44.567 ms | 46.895 ms |

The execution-time target passes in all three runs. These timing values
exclude WAV/frontend computation, cold restore and endpoint latency; they
are not final ASR end-to-end latency claims. Power/scheduling variation is
visible, so the three results are retained rather than reporting only the
fastest sample.

```powershell
python -B "$Tools\nemotron_m1_run.py" benchmark `
    --contexts "$Data\m1\automated-full-run" `
    --fixture "$Data\m1\full-cases-v2-fp16\long.nmf" `
    --output "$Data\m1\new-benchmark" --repeats 3

& $Python -B "$Tools\nemotron_m1_run.py" --deps-dir $Deps audit `
    --fixture "$Data\m1\quant16-axis-full-cases\long.nmf" `
    --result "$Data\m1\quant16-axis-full-run\long.nmr" `
    --output "$Data\m1\new-independent-audit.json"
```

The persisted investigation is
`D:\newos-data\nemotron-asr\m1\m1-investigation-2026-10-14.json`; timing evidence
is under `m1\benchmark-fp16`. Original failed experiments and M0 evidence are
preserved. **M1 remains blocked on numerical parity, not execution time.**
Passing it requires a materially different, validated precision strategy;
silently widening tolerances, replacing the FP32 reference, teacher-forcing
caches or hiding CPU inference is not a fix.
