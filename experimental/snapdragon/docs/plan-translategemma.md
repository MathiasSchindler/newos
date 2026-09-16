# TranslateGemma 4B direct-QNN implementation plan

## Objective

Run the text-only language backbone of `google/translategemma-4b-it` on the
Snapdragon X Elite HTP with useful interactive translation speed. The deployed
program remains freestanding ARM64 C, uses no CRT or standard C library, imports
only Windows system APIs statically, and calls QNN directly. Python, PyTorch, and
NumPy are permitted only for pinned model acquisition, conversion, calibration,
and reference generation.

The first release supports text-to-text translation. The SigLIP vision tower and
image translation are explicitly deferred.

## Runnable 512-token prototype (2026-09-16)

Build from the repository root with
`./experimental/snapdragon/tools/build-gemma.ps1 -Translate`. The VS Code task
`TranslateGemma build inline` runs the same build without changing PowerShell's
machine execution policy. Run the resulting executable:

```powershell
.\experimental\snapdragon\build\translate.exe --from de --to en "Guten Tag, mein Name ist Hase. Ich weiß von nichts."
```

Observed output: `Good day, my name is Hase. I know nothing.`

This is an experimental row-W4, greedy, text-only NPU prototype, not Stage 8
performance or translation-quality acceptance. It restores the existing verified
128-row Stage 7 graph for prefill. One-off calls also use it for decode to avoid
loading a second context; `--decode` explicitly selects the dedicated one-row
decoder. `--batch` selects one-row decoding automatically. Prompt plus reserved
output must fit 512 tokens; `--max-tokens` defaults to 64 (allowed 1..256). Stop
tokens are omitted. Exit 0 means a nonempty EOS-terminated result, exit 1 means
failure, and exit 2 means the output budget was exhausted; partial output is still
printed. Known row-W4 semantic failures remain unresolved.

The executable is freestanding ARM64 C, has only Kernel32 static imports, and
loads QNN dynamically. Python is not used at runtime. Translation alone goes to
stdout (UTF-8 when redirected, Unicode console output otherwise); diagnostics go
to stderr. The prototype textually reuses the tested block runner for context
validation, tensor registration, and cleanup; it does not load expected KV/logit
fixtures or invoke a CPU model.

Keep the executable under `experimental/snapdragon/build`, alongside the matching
QAIRT 2.50/QNN 2.39 DLLs, HTP skeletons and their `.cat` files. Defaults resolve
relative to the executable: `gemma-block/prompt-512.gmb` (with its `.context` file)
and `../models/translategemma-4b-stage4/tokenizer.gta`. The binding references the
existing validated W4 embedding and Stage 7 RoPE artifacts using absolute ASCII
paths. `--bindings PATH` and `--tokenizer PATH` override defaults. Model asset
paths must remain ASCII; source text supports Unicode. The build does not export
weights or create a missing QNN context with `-Translate` alone. Build the
one-row context once using `build-gemma.ps1 -BuildDecode`; it writes
`build/gemma-block/prompt-512.gmb.decode.context` (1,964,562,520 bytes). It is
validated independently against a one-row I/O schema and SHA-256 digest.

The 20-case bounded CLI/NPU check is
`experimental/snapdragon/build/calibration-venv/Scripts/python.exe -B experimental/snapdragon/tools/test-translate.py --hardware`.
Without `--hardware`, only request validation runs. Results are saved beside the
executable in `translate-optimized-test-results.json`. Hardware cases cover the sentence
above, greeting, embedded quotes, German umlaut output, truncation, and successful
QNN cleanup; malformed initial requests are rejected before context restore.
Additional cases cover batch isolation, EOF without a newline, batch truncation,
invalid UTF-8 in a subsequent request, opt-in QNN profiling, and per-step
KV/logit parity against the padded graph for both 89- and 196-token prompts.

Before optimization, on this Snapdragon X Elite, the sentence took 16.54 seconds wall time: context
creation from validated binary 1.10 seconds, prefill 0.25 seconds, and 12 decode
steps 2.93 seconds (about 4.1 steps/s). File loading, complete payload hashing, and
other startup work accounted for most remaining latency. The earlier 253-token incremental probe
passed all 34 layers' KV and final-logit tolerances. The stopped CPU quality sweep
and its preserved results were not restarted or changed.

## Prototype profile (2026-09-16)

This section records the **pre-optimization baseline**; current results and
commands are in the optimization section below. The baseline report remains
unchanged. The current profile task tests three explicit one-row runs, three
automatic one-off runs, and a three-request resident batch.

Build with `build-gemma.ps1 -ProfileTranslate`, or run the VS Code task
`TranslateGemma profile` to build and measure three sequential invocations. This
creates a separate `build/translate-profile.exe`, leaving `translate.exe`
unchanged. `test-translate.py --profile` checks identical output/token IDs,
successful cleanup, phase accounting, and agreement with parent-process wall
time. Raw measurements are in `build/translate-profile-results.json`; normal CLI
test results are not overwritten. Profiling uses QueryPerformanceCounter and
keeps the freestanding ARM64/Kernel32-only contract. Normal builds compile out
the counters.

Three runs of the Hase sentence (89 prompt tokens, 12 text tokens plus EOS) took
16.31..16.94 seconds wall time. Median in-process time was 16.51 seconds, with the
first selected token at 12.93 seconds. Filesystem caches were not flushed, power
settings were unchanged, and every invocation loaded a fresh QNN context.

| Measured region | Median time |
| --- | ---: |
| Context payload SHA-256 | 8.794 s |
| Embedding and RoPE artifact validation/hashing | 1.414 s |
| Context allocation/read | 0.673 s |
| Embedding and RoPE file reads | 0.134 s |
| QNN context creation from validated binary | 1.012 s |
| Prefill graphExecute (89 valid rows of 128) | 0.233 s |
| Decode graphExecute (12 calls, one valid row each) | 2.781 s |
| Decode embedding/mask preparation, all calls | 0.064 s |
| Decode guards/KV copying, all calls | 0.011 s |
| Argmax, 13 selections | 0.037 s |
| Cleanup | 0.578 s |

These selected regions omit smaller startup work; medians need not sum exactly.
The raw report also contains parent regions (`restore_total`, `embedding_load`,
`buffers`); do not add their nested read/hash measurements a second time.
Exclusive phase accounting covered at least 98% of each invocation.

About 62% of elapsed time is integrity hashing, using the scalar SHA-256 path
under `-Oz`. Actual file reads are much smaller. QNN graph calls occupy about
18% of the complete invocation, explaining the brief high-NPU-usage interval.
Within generation, about 96% is already inside the blocking QNN call, not Python,
tokenization, mask construction, or KV copying. Typical decode calls took
230..234 ms, with one 299 ms outlier across the three runs. A graph call is host
wall time including QNN dispatch/transfers/synchronization; this profile does
not measure DSP kernel time, hardware utilization, DDR traffic, or per-op costs.

Optimization priorities based on these measurements:

1. Validate and enable an ARM SHA-256 implementation, preserving every integrity
   check. The repository has an opt-in ARM path, but this build does not enable
   it; known-answer and corruption tests are prerequisites, not an assumed pass.
2. Keep the validated context, embeddings and tokenizer resident for multiple
   requests, amortizing hashing/loading/cleanup instead of repeating them.
3. Build a dedicated one-row decode graph. Current prefill and decode calls cost
   almost the same despite 89 versus one valid token. Masked rows still have
   fixed-shape computation; a 128x speedup must not be inferred because weight
   bandwidth and dispatch costs remain.
4. Profile QNN operations/transfers once that graph exists. Host preparation and
   argmax are secondary targets (roughly 0.11 seconds across this generation).

No optimization, integrity bypass, model change, or quality-campaign restart was
performed in this profiling pass. Existing translation-quality limitations remain.

## Implemented optimizations (2026-09-16)

- Enabled ARM SHA instructions in the Snapdragon-specific build, fixed the
   accelerated SHA round's original-state dependency, and kept full blocks on
   the direct hashing path after a partial header. No integrity checks were
   removed. Scalar and ARM SHA both pass empty, abc, multi-block, and split-update
   million-byte known-answer vectors plus existing artifact tests.
- Added resident `--batch` mode: UTF-8 input, one nonempty source line per request,
   fixed `--from`/`--to`, EOF ends the process. Tokenizer, embeddings, QNN contexts,
   RoPE tables and registered buffers stay loaded; request scratch memory is
   released and KV state is reset between translations. Output is flushed directly
   after each request. Blank/invalid/over-budget input fails the batch with exit 1;
   any truncated result makes its final status 2. Generated text may itself contain
   newlines, so stdout is not a structured response protocol. This is a piped-input
   mode, not an interactive Unicode-console REPL.
- Added a validated one-row decode context and valid-prefix KV handoff from
   prefill. Both graphs stay resident in batch mode. `--decode` forces this path;
   `--padded-decode` retains the single-graph reference path. Default one-off calls
   use one context because loading the extra roughly 1.96 GB context costs more
   than it saves on the measured short sentence. A one-token output budget also
   skips the unused decode context.
- Built translator host code with `-O2`: mask preparation, cache clearing/copying,
   and argmax overhead fell substantially. No dependency or numerical contract was
   changed. Windows no-CRT ARM64/Kernel32-only audits still pass.

Example resident usage from PowerShell (stdin must be UTF-8):

```powershell
$OutputEncoding = [Text.UTF8Encoding]::new($false)
@('Guten Tag.', 'Die Tür ist offen.', 'Guten Tag.') | .\experimental\snapdragon\build\translate.exe --from de --to en --batch
```

Final profile on this machine, same Hase sentence and unchanged power settings:

| Measurement | Baseline | Optimized |
| --- | ---: | ---: |
| One-off median wall time | 16.52 s | 8.00 s |
| One-off context hashing | 8.79 s | 1.04 s |
| One-off first token selected (median) | 12.93 s | 4.19 s |
| QNN decode call, dedicated graph | 232 ms padded | 148 ms one-row |
| Resident request, after startup | unavailable | 2.07..2.11 s |

The three optimized one-off runs ranged from 7.84 to 9.29 seconds; cache state
was not forced cold. Explicit two-context one-off mode took about 13.13 seconds
in-process, so it is deliberately not the default. Three resident requests took
16.37 seconds total including both-context startup and cleanup. Decode throughput
is about 6.6 generated steps/s including host work (about 6.8 QNN calls/s), not the
Stage 8 target of 10+ tokens/s. Quality remains the same experimental row-W4 model.

Further investigation performed after those optimizations:

- Read-only memory-mapped context loading was measured and rejected: 13.77 s
   median versus 13.05 s buffered for explicit two-context runs. Page faults moved
   into the hashing phase and negated copy savings. The mapping code was removed.
- `--qnn-profile` samples QNN basic events for the first prefill and decode calls.
   A decode sample reported 143.8 ms accelerator time excluding wait in a 148.2 ms
   QNN call, with four HVX threads. This supports a device-work bottleneck rather
   than large CPU/RPC gaps; it does not establish DDR bandwidth, HMX utilization,
   frequency, or individual operator cost. Profiling output is on stderr and
   disabled by default. `--verify-decode` runs both graphs at every decode step
   and checks all layers' KV rows and logits using unchanged tolerances; it is
   diagnostic, not a benchmark mode.
- The next substantial candidates are a combined context with shared weights
   (avoiding duplicate restore/storage), detailed per-operator profiling to locate
   expensive MatMuls/head/attention transforms, and measured QNN performance-mode
   tuning with power/thermal tracking. More CPU-loop tuning alone cannot remove
   the measured accelerator cost. No global power settings were changed.

Current raw reports: `build/translate-optimized-profile-results.json` and
`build/translate-optimized-test-results.json`. The original baseline reports and
stopped quality campaign remain untouched. Gates passed: 20 CLI/hardware cases,
15 stepwise all-layer KV/logit comparisons, 150 prompt/decode envelope corruption
cases, 7,470 tokenizer cases, 18 tokenizer corruptions, 323 numerical scalar
fixtures, scalar and ARM SHA known-answer vectors, strict compilation and PE
audits. Build integrity gates with `-Test -TestEnvelope -TestNumerics`; add
`-ScalarHash -Test` to check the scalar hash path without deploying it.

## Streaming and renewed profiling (2026-09-16)

The translator now writes stable decoded text as tokens arrive, on both consoles
and UTF-8 pipes. Use `--quiet` for translated text only:

```powershell
.\experimental\snapdragon\build\translate.exe --quiet --from de --to en 'Guten Tag.'
```

`--no-stream` restores whole-result buffering. Both modes append one newline per
request. Streaming holds trailing byte-fallback runs until decoding is stable,
so incomplete UTF-8 never becomes premature replacement characters. Final output
matches the existing decoder, including its malformed-byte replacement rules.
Quiet mode suppresses application diagnostics and backend stderr through cleanup;
use exit status 0 for completion, 1 for failure, and 2 for a token-limit result.
Omit quiet mode when diagnosing failures. A failure after streaming starts can
leave partial text; token-limit results are also partial translations. Pipe
consumers may introduce their own buffering. Quiet mode also suppresses profiling.

Three interleaved streamed/buffered runs of the same Hase sentence, quiet mode:

| Measurement | Streamed | Buffered |
| --- | ---: | ---: |
| Median first stdout byte | 4.06 s | 6.80 s |
| Median process wall time | 7.57 s | 7.46 s |
| First-to-last stdout byte span | 2.84-2.85 s | under 0.2 ms |

This is a responsiveness improvement, not evidence of faster token generation.
Startup variation exceeds the output cost. A separate three-run instrumented
one-row profile measured 0.554 ms total output work for the sentence, 147 ms per
decode execution, and 13.6 ms total CPU argmax work. Three resident requests took
2.031-2.047 s each, including prefill, with decode calls at 144.6-149.8 ms.
Two-context startup remains expensive: median combined context read/hash/create
times were 2.30/2.17/2.48 s; cleanup was 1.12 s. Filesystem cache and machine state
affect these numbers; they are not a cold-start benchmark. Integrity checks remain
enabled, and no power settings were changed.

`--qnn-profile-detailed` requests QNN level 2 and samples the first prefill and
decode executions. It produced 1,444 events per sample from the existing cached
graphs. The decode sample reported 609.0 million accelerator cycles. Dominant
depth-1 operation groups were:

| Graph event group | Million cycles | Fraction of accelerator cycles |
| --- | ---: | ---: |
| GELU-labelled events, 34 layers | 134.2 | 22.0% |
| MLP up projections, 34 layers | 133.7 | 22.0% |
| MLP down projections, 34 layers | 110.4 | 18.1% |
| Vocabulary projection | 101.7 | 16.7% |
| Attention QK products, 34 layers | 32.7 | 5.4% |

These are backend event attributions, not isolated source-node timings. In
particular, gate-projection events are absent and the graph places GELU immediately
after that projection, so the GELU label may include fused projection work. Do not
infer that standalone activation math costs 22%. Detailed profiling adds overhead;
its cycle shares must not be converted to unprofiled milliseconds or treated as
HMX utilization, DDR bandwidth, or clock-frequency measurements.

Highest-value next experiments:

1. A combined prefill/decode context with shared weights, to reduce duplicate
   reads, hashing, restore work and memory pressure. Deferring decode restore until
   after the first streamed token could improve explicit `--decode` first-output
   latency even without reducing total work.
2. Inspect backend placement/fusion for gate/up/down projections, then benchmark
   a paired gate/up projection or supported fused MLP representation against the
   existing numerical oracle. The MLP groups account for about 62% of this sample.
3. Benchmark the full-vocabulary matrix-vector kernel and weight layout. Merely
   moving argmax onto the NPU does not remove the expensive projection; vocabulary
   pruning would change behavior and is not an equivalent optimization.
4. Test supported QNN performance settings with power/thermal measurements. More
   CPU-loop tuning alone cannot remove the dominant device work.

No graph/quantization changes were made in this pass. W4 quality limitations and
the stopped quality campaign are unchanged; production Whisper is untouched.
Validation passed: 7,470 tokenizer fixtures with stable-prefix checks on every
successful case, three explicit byte-fallback streaming cases, 18 corruptions,
four SHA vectors, 20 existing CLI/hardware cases (including 15 all-layer KV/logit
parity steps), and 12 quiet/streaming/detailed-profile cases. The freestanding
build still imports only Kernel32 and has no exception or CLR tables.

Reproduce with `test-translate.py --streaming`, `--hardware`, and `--profile` after
building the corresponding normal/profile binaries. Reports are
`build/translate-streaming-results.json`, `build/translate-streaming-test-results.json`,
and `build/translate-streaming-profile-results.json`; earlier reports are preserved.

## Shared-weight and NPU-first implementation (2026-09-16)

The installed translator automatically uses `gemma-block/prompt-512.gmb.bundle.context`
when available for requests reserving more than one output token. The bundle
contains both 128-row prefill and one-row decode graphs with QNN HTP weight sharing
enabled. Its binary payload is 1,980,047,360 bytes, versus roughly 1.96 GB for each
of the previous independent contexts. Both graph schemas, exact model/runtime
identity, payload length and SHA-256 are checked before restore. An installed but
invalid bundle fails rather than silently falling back. Explicit `--bundle`
requires it; `--decode` and `--padded-decode` retain independent-context references.
Single-token requests retain the previous single-context path.

Both bundled graphs reuse the registered past-KV memory. This removes about 68 MiB
of duplicate cache storage and the CPU prefill-to-decode cache copy. Current KV
rows still require CPU insertion into the cache; embedding preparation, masks,
tokenization and integrity hashing also remain host-side. This is not an entirely
CPU-free inference path.

Greedy selection and finite-logit validation now execute on the NPU by default in
bundle mode, in a small separate graph built once per process. The backend's
`Argmax` failed the first-maximum tie rule (all-zero input selected ID 65472), so
the implementation uses ReduceMax, equality, index selection and ReduceMin instead.
Tests cover ties, final vocabulary ID, NaN and both infinities. `--verify-decode`
cross-checks against the CPU selector, and compares all-layer KV rows and logits.
`--cpu-selection` selects the lower-dispatch-overhead reference. Full logits are
still exposed by the model graph for diagnostics, so their transfer is not removed.

Measurements on this machine, subject to cache and background-load variation:

- The first shared bundle measured 6.28 s median for the Hase sentence versus the
   earlier independent one-row path's 10.99 s. This gain is primarily loading and
   shared storage, not a faster matrix kernel.
- Final deployed NPU-selection runs measured 6.50 s median in quiet streaming
   tests, and 6.88 s in the explicit bundle test. First output was about 4.05 s
   and 4.37 s respectively. The prior quiet single-context median was 7.57 s;
   these are separate passes, not a controlled same-time A/B comparison.
- Final resident requests took 2.046-2.064 s; model decode remains about 147 ms.
   Graph-side selection costs about 3-4 ms per token including dispatch, versus
   roughly 1-1.5 ms for CPU selection. It reduces CPU scanning, not wall time.
   Its per-process graph construction adds about 0.35 s of startup work.
- One bundle restore reads/hashes/creates once. Representative final timings were
   0.67/1.07/1.18 s, with cache transfer bookkeeping below 0.2 ms per request.

Projection experiments covered both the MLP and vocabulary head. Row-major W4
weights plus MatMul `transpose_in1` passed local/global block oracles and full
translation checks, but detailed decode cycles stayed about 610 million versus
609 million before. This layout is used by the installed bundle, without a claimed
kernel speedup. `FullyConnected` also passed block gates, but its full-model first
graph finalization exceeded eleven minutes and was stopped; it is not deployed.
`-FullyConnected` remains an explicit experimental build switch. Fused/paired MLP
projection and a genuinely faster vocabulary kernel remain unimplemented targets.

`--performance` requests a process-scoped HTP performance vote, released during
cleanup. Three interleaved batches per mode showed essentially unchanged resident
latency (about 2.04 s/request), so it is not enabled by default. No Windows-wide
power policy was changed. Power draw and thermal telemetry were not measured;
no efficiency or sustained-frequency improvement is claimed.

Rebuild the bundle with `build-gemma.ps1 -RowMajorProjections -BuildBundle` after
preparing the existing prompt binding and QNN runtime files. Normal `-Translate`
builds consume the installed bundle. `-NpuSelection -BuildBundle` is an experimental
in-model selector variant, not the deployed separate-selector artifact. The build
can log a QNN weight-mapping warning while holding construction graphs; the saved
bundle was separately restored and executed successfully in fresh processes.

Gates passed: 150 legacy envelope plus 14 bundle corruption cases, four file-level
bundle truncation/corruption cases, six NPU selector cases, local/global W4 block
oracles and cleanup injection, 20 deployed CLI cases, 12 streaming/quiet cases,
full bundle multi-chunk/Unicode/original-context checks, 7,470 tokenizer fixtures,
18 tokenizer corruptions, 323 numeric fixtures, four SHA vectors, strict compiler
checks and Kernel32-only/no-CRT PE audits. Quality campaigns remain stopped and
Whisper is unchanged. Reports: `build/translate-bundle-results.json`, current
`build/translate-streaming-*.json`, and
`build/row-major/translate-performance-results.json`. The streaming-named reports
are refreshed by their test modes; they are not immutable historical snapshots.

## Feasibility baseline

The official checkpoint is a Gemma 3 conditional-generation model. Its text
backbone contains exactly 3,880,263,168 BF16 parameters; the complete stored
checkpoint has 4,300,079,472 parameters after including the vision tower and
projector. The text backbone uses:

| Property | TranslateGemma 4B text backbone |
| --- | ---: |
| Decoder layers | 34 |
| Hidden width | 2560 |
| MLP width | 10240 |
| Query heads | 8 |
| KV heads | 4 |
| Head width | 256 |
| Vocabulary | 262208 |
| Translation context | 2048 tokens |
| Attention pattern | Five local layers per global layer |
| Local window | 1024 tokens |

BF16 text weights require 7,760,526,336 bytes (about 7.23 GiB) before QNN context and runtime
overhead. That is not a practical deployment format on the target 16 GiB
machine. The production target is symmetric weight-only INT4 with FP16
activations (`W4A16`); INT8 is a bring-up and quality-control format.

At the 2048-token deployment context, an FP16 KV cache requires about 156 MiB
when the 29 local layers retain 1024 tokens and the five global layers retain
2048. KV storage is still not the primary memory risk; duplicated weights in
prompt and token-generator contexts are.

Comparable Qualcomm AI Hub 4B models already use `q4_0w4a16` on Snapdragon X
Elite. This establishes platform feasibility, but it is not a performance result
for TranslateGemma or for this direct-QNN implementation.

## Constraints

- Runtime code is C11, freestanding, no CRT, and no libc.
- `KERNEL32.dll` remains the only static DLL dependency of the inference tool.
- `QnnHtp.dll`, `libcdsprpc.dll`, HTP stubs/skels, and firmware remain unavoidable
  dynamically loaded platform dependencies.
- The model, tokenizer, and serialized QNN contexts are external validated data,
  not linked into the PE image.
- ONNX Runtime, Genie, llama.cpp, and Transformers may be reference tools but are
  not part of the deployed process.
- The model revision, original hashes, conversion settings, runtime version,
  graph contract, and generated artifact hashes are pinned.
- No proprietary Qualcomm headers or binaries are committed.
- Every optimization is accepted on end-to-end latency, memory, and translation
  quality, not NPU utilization alone.
- Fixed-shape graphs and model-specific context binaries are preferred over a
  general runtime graph interpreter.

## Non-goals

- Supporting the 12B or 27B variants in the first implementation.
- Supporting image translation or the SigLIP vision encoder.
- Reimplementing a general Safetensors, Jinja, or Hugging Face runtime in the
  deployed binary.
- Loading arbitrary Gemma-family checkpoints without conversion and validation.
- Matching every Transformers sampling option. Deterministic greedy generation
  is the initial contract.
- A full CPU implementation of the 4B model in the production executable.

## Stage 0: Source-tree cleanup

**Status: complete (2026-09-13).**

Reorganize `experimental/snapdragon/src` before adding another model. This stage
is a mechanical ownership change, not a runtime refactor.

Target layout:

```text
src/
  shared/
    imports/
      kernel32.def
      dxcore.def
      d3d12.def
      directml.def
    tests/
      qnn_mock.c
    qnn_abi.h
  tools/
    probe/
      main.c
    whisper/
      main.c
      whisper_artifact.c
      whisper_artifact.h
      whisper_decoder.c
      whisper_decoder.h
      whisper_decoder_qnn.c
      whisper_decoder_qnn.h
      whisper_encoder_qnn.c
      whisper_encoder_qnn.h
      whisper_frontend.c
      whisper_frontend.h
      whisper_model.c
      whisper_model.h
      benchmarks/
        decoder_kernel_benchmark.c
      tests/
        whisper_artifact_test.c
        whisper_decoder_cleanup_test.c
    gemma/
      ... added by later stages
```

Actions:

1. Move files with history-preserving renames and keep public executable names
   unchanged: `probe.exe`, `npu_probe.exe`, `npu_probe_builder.exe`, and
   `decoder_kernel_benchmark.exe`.
2. Move only the already model-independent QNN ABI, import definitions, and mock
   provider into `src/shared`. Keep `whisper_artifact.*` under Whisper because its
   current header embeds Whisper-specific dimensions and payload kinds.
3. Update `build.ps1`, `test-npu-probe.ps1`, exporter references, include paths,
   and documentation links.
4. Do not split `npu_probe.c`, rename symbols, change artifact formats, or extract
   new abstractions in the same change. Those edits would make regressions harder
   to attribute.
5. Reserve `src/tools/gemma` as the owner of all TranslateGemma-specific runtime
   code. The directory becomes concrete when Stage 2 adds its first model files;
   do not add an empty placeholder file.

Exit criteria:

- All four existing binaries build with unchanged names.
- The artifact and decoder cleanup tests and all mock-provider cases pass.
- The established 35-second Small transcript remains unchanged.
- `npu_probe.exe` remains ARM64, has no exception or CLR tables, and imports only
  `KERNEL32.dll`.
- No generated artifact or QNN context format changes.
- `git diff --check` passes.

Implementation record:

- The shared QNN ABI, import definitions, and mock provider now live under
   `src/shared`; the probe and Whisper implementation now live under `src/tools`.
- All four binaries retain their original names and pass a clean build.
- The artifact contract, decoder cleanup, and all mock-provider cases pass.
- A rebuilt Small context restores and executes on the HTP, and the focused
   35-second quiet transcript matches the established output.
- The production PE remains ARM64 with no exception or CLR tables and with
   `KERNEL32.dll` as its only static import.

## Stage 1: Shared QNN substrate and runtime upgrade

**Status: complete (2026-09-13).**

QAIRT 2.50/QNN core 2.39 is now the single validated working runtime. Qualcomm's
comparable 4B packages require QNN SDK 2.45 or newer, so this satisfies the SDK
floor without keeping a parallel legacy installation.

The official archive is hash-pinned, the narrow ABI and provider gate require
QNN core 2.39, the ARM64 HTP runtime is staged from that archive, all Whisper
contexts were regenerated, and the Small transcript gate passed.

Actions:

1. Acquire the exact SDK headers and runtime under their license and record the
   archive version and hashes outside Git.
2. Extend `src/shared/qnn_abi.h` from the exact headers. Preserve compiler-checked
   ARM64 structure sizes, offsets, enum values, and function-table slots.
3. Add narrowly shared QNN loader/lifecycle and context-cache helpers only after
   both Whisper and Gemma call sites are known. Keep graph construction
   model-specific.
4. Retest every existing Whisper graph and context under the upgraded runtime.
5. Add capability probes for the operations and data contracts needed by Gemma:
   W4A16 FullyConnected, RMSNorm, gated GELU, RoPE arithmetic, Gather, grouped
   query attention shapes, causal masking, shared-memory KV tensors, and
   ArgMax/TopK if their public ABI is available.

Exit criteria:

- Existing Whisper output and failure-path behavior remain accepted.
- The narrow ABI is derived from and checked against the pinned SDK.
- A model-shaped W4A16 projection passes a scalar or high-precision reference.
- Unsupported operations have an explicit host-side or graph-composition
  fallback before full-model work starts.

Implementation record:

- The capability suite is builder-only and runs with
   `tools/test-gemma-stage1.ps1`; the production `npu_probe.exe` neither links the
   probe module nor accepts its option.
- A representative `[1,2560] x [2560,2560]` FP16 activation/W4 projection agrees
   with the scalar reference. Native packed `QNN_DATATYPE_SFIXED_POINT_4` is rejected by this HTP
   provider. The accepted construction contract uses signed 8-bit build-time
   storage with `QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET`, bit width 4,
   axis 1, and per-output-channel scales. Production artifacts may remain packed
   W4 and are unpacked only while constructing the QNN context.
- Direct FP16 RMSNorm, RotaryEmbedding, Gather, Argmax, and TopK pass numerical
   checks. Gated GELU passes as `Gelu` followed by `ElementWiseMultiply`.
- The public `GroupQueryAttention` node accepts graph construction but the
   Windows HTP provider rejects it during finalization with error 1002. The
   explicit fallback expands each KV head for its query-head group and executes
   MatMul, causal-mask addition, Softmax, MatMul, Transpose, and Reshape on HTP.
   The model geometry of 8 query heads, 4 KV heads, and head width 256 passes a
   two-token causal numerical reference.
- Two FP16 KV tensors of shape `[1,4,2048,256]` are allocated in one `rpcmem`
   region, registered as separate QNN shared-buffer handles, consumed and
   produced by an HTP graph, and compared exactly after execution.
- Shared lifecycle extraction remains deferred until the Gemma runtime provides
   the second production call site. Stage 1 adds only the ABI required by the
   probes and keeps graph construction model-specific.

## Stage 2: Pinned model descriptor and acquisition

**Status: complete (2026-09-13).**

The descriptor, catalog, authenticated fetch path, strict configuration and
Safetensors validator, deterministic tensor audit, and local failure fixtures
are implemented. An authenticated official download under
`data/translategemma-4b` matches the pinned revision across all Git blob IDs,
LFS SHA-256 values, and file sizes. The complete checkpoint passes configuration,
index, shard-header, tensor-selection, and inventory validation. Its local
`source-lock.json` and `tensor-audit.json` are generated data and remain outside
Git with the licensed model files.

Materialize `src/tools/gemma` with a canonical descriptor for TranslateGemma 4B.

Actions:

1. Add `gemma_model.h/.c` with model identity, layer types, dimensions, vocabulary,
   context limit, RoPE parameters, normalization epsilon, activation, token IDs,
   and artifact limits.
2. Add a model catalog under `tools/` containing the accepted Hugging Face
   revision, expected files, sizes, and SHA-256 hashes.
3. Require the user to accept the Gemma license and provide authenticated access;
   never embed credentials in scripts or manifests.
4. Download atomically with resume support and verify every file before use.
5. Validate the checkpoint configuration against the compiled descriptor.
6. Select only language-model tensors for text translation. Record every omitted
   vision and projector tensor so omission is auditable.

Exit criteria:

- A fresh authenticated fetch is reproducible from the pinned catalog.
- Configuration drift, missing shards, unexpected tensors, truncation, and hash
  mismatch fail before conversion.
- The exact retained parameter count and raw byte count are recorded.

Implementation record:

- `src/tools/gemma/gemma_model.h/.c` pins the 34-layer, 2560-wide text descriptor,
   the 5-local/1-global schedule, and the immutable source revision.
- `tools/translategemma-models.json` records all 15 official files with complete
   SHA-256 pins. Git-stored files also retain their official blob SHA-1 identities,
   while LFS files retain their official object SHA-256 identities.
- `tools/fetch-translategemma.ps1` requires `-AcceptGemmaLicense`, reads a token
   only from the environment or standard Hugging Face cache, verifies the exact
   revision, resumes into a staging directory, and publishes only after validation.
- `tools/validate-translategemma.py` rejects catalog, file, configuration,
   index, shard-header, namespace, shape, offset, and inventory drift without
   loading tensor payloads into memory.
- The source shard headers contain 883 tensors. Text selection retains 444
   `language_model.*` tensors totaling 3,880,263,168 parameters and 7,760,526,336
   raw bytes. It reports and omits 439 `vision_tower.*` or
   `multi_modal_projector.*` tensors totaling 419,816,304 parameters and
   839,632,608 raw bytes.
- `tools/test-gemma-stage2.ps1` runs the descriptor and acquisition gates;
   its `test-translategemma-stage2.py` suite covers accepted selection, configuration
   drift, file corruption, truncation, shard-map drift, unexpected namespaces,
   and missing files using local fixtures.

## Stage 3: Versioned artifacts and W4A16 conversion

**Status: complete (2026-09-13).**

Create a Gemma-specific artifact contract rather than extending Whisper headers
with unrelated fields.

Artifact classes covered by the contract:

- W4 language-model weights and per-group scales.
- FP16 normalization and other small parameters where quantization is not useful.
- Pinned tokenizer source data and, in Stage 4, generated vocabulary,
   normalization, byte-mapping, and lookup tables.
- Layer-type and RoPE tables.
- Deterministic prompt, layer-output, logits, and generated-token fixtures from
   Stage 5.
- Prompt-processor and token-generator QNN context binaries from Stages 7 and 8.

Actions:

1. Export from the original BF16 checkpoint, not a third-party GGUF conversion.
2. Start with W8A16 as a numerical bring-up control, then implement symmetric
   W4A16 using the exact grouping and packing accepted by HTP.
3. Record deterministic per-tensor quantization error during conversion. Stage 5
   applies the multilingual translation quality gate to the resulting W8 and W4
   artifacts.
4. Use a little-endian versioned header with model identity, source revision,
   tensor name or stable ID, shape, layout, element and quantization types, group
   size, payload size, and a strong payload hash.
5. Publish complete artifact sets atomically through a manifest written last.

Exit criteria:

- Every tensor round-trips through the artifact reader and agrees with the
  exporter reference.
- Wrong model, version, dimensions, layout, quantization, truncation, overflow,
  and payload corruption are rejected before QNN binding.
- W4 weight and scale storage is in the expected 1.9-2.2 GB range for the text model.

Implementation record:

- `src/tools/gemma/gemma_artifact.h/.c` defines a 256-byte little-endian v1
   per-artifact header with model and tensor identity, rank and dimensions,
   layout, element and quantization types, grouping, byte ranges, and SHA-256.
   The reader rejects reserved-field, model, name, dimension, overflow, size, and
   payload corruption before binding and expands low-nibble-first signed W4 into
   the QNN S8 build container.
- `tools/export-translategemma.py` revalidates the pinned Stage 2 checkpoint,
   streams BF16 tensors in bounded row chunks, uses ties-to-even symmetric
   quantization, records per-tensor RMSE and maximum absolute error, and writes
   the manifest only after every artifact has been reread and SHA-256 checked.
- QAIRT mapped group-128 encoding finalized and executed but returned an
   unscaled integer dot product; its float-block alternative failed inside QNN
   finalization. The deployed contract therefore uses the measured working path:
   one scale per output channel, S8 build storage with bit-width 4 and axis 1.
   The corrected 2560-wide hardware probe passes this contract on HTP.
- The published ignored artifact set under `models/translategemma-4b-stage3/`
   contains 444 W8A16 tensors with 3,883,175,040 payload bytes and 444 W4A16
   tensors with 1,943,227,520 payload bytes. W4 matrices and their scales occupy
   1,942,491,264 bytes; rank-1 FP16 tensors occupy 736,256 bytes.
- Six pinned tokenizer files, the 34-byte local/global layer schedule, RoPE
   constants, and the quantization contract are wrapped in the same format.
   Runtime-ready tokenizer tables, numerical model fixtures, and QNN contexts are
   deliberately named as deferred outputs in the manifest and are produced by
   their owning later stages rather than fabricated here.
- `tools/test-gemma-stage3.ps1` runs the C reader/corruption/unpack checks and
   Python streaming quantization tests. `tools/export-translategemma.py
   --verify-only` independently verifies the complete published inventory.

## Stage 4: Freestanding tokenizer and prompt contract

**Status: complete (2026-09-15).**

Implement only the tokenizer and template behavior required for TranslateGemma.

Actions:

1. Convert the pinned BPE vocabulary, merge ranks, added tokens, and language
   table into a bounded binary representation suitable for direct C lookup.
   The pinned fast tokenizer uses BPE, not unigram segmentation; its JSON and
   tokenizer configuration define the reference contract. Parse neither JSON nor
   protobuf in the deployed C code.
2. Implement UTF-8 validation, normalization required by the pinned tokenizer,
   ranked BPE merging, byte fallback, special tokens, and detokenization.
3. Implement the fixed TranslateGemma text prompt directly in C from validated
   source and target language identifiers. Do not embed a Jinja interpreter.
4. Reject unsupported language identifiers and inputs that would exceed the
   2048-token deployment contract, including the prompt and reserved output budget.
5. Compare token IDs and decoded bytes against the pinned Transformers tokenizer
   for ASCII, multilingual scripts, combining characters, malformed UTF-8,
   punctuation, and byte-fallback cases.

Exit criteria:

- Token IDs are identical to the reference corpus.
- Detokenized bytes are identical and valid UTF-8.
- Tokenization has deterministic allocation bounds and no CRT imports.

Implementation record:

- `src/tools/gemma/gemma_tokenizer.h/.c` provides an allocation-free table reader,
   heap-based BPE encoder, UTF-8 decoder, and fixed text prompt builder. The
   immutable table payload is 14,790,893 bytes; the caller-owned workspace is
   5,701,636 bytes. Input bytes are capped at 196608 and output tokens at 2048.
- The tokenizer defines 262145 IDs, while the model has 262208 output slots.
   Decoding unmapped IDs fails. Source spaces normalize to U+2581; there is no
   extra NFC/NFKC transformation. Added tokens are recognized before normalization.
   Configured extra-special-token flags match the pinned Transformers loader.
- Prompt generation supports the source template's 581 exact language identifiers,
   underscore-to-hyphen normalization, Unicode whitespace trimming, and exact
   prompt wording. The prompt token count plus requested output budget must fit
   the deployment context. Malformed input UTF-8 is rejected; invalid generated
   byte-fallback runs decode to the same replacement bytes as the reference.
- `gemma_model_is_stop_token` recognizes both 1 (`<eos>`) and 106
   (`<end_of_turn>`). Greedy reference generation must explicitly disable the
   source generation configuration's sampling default.
- The existing `tools/export-translategemma.py --tokenizer-only` path exports
   tables using only Python's standard library. `--tokenizer-reference` additionally
   requires pinned Transformers 4.57.3, Tokenizers 0.22.2, and Jinja 3.1.6 to
   regenerate fixtures. Both artifacts are reread and hashed before an atomic
   manifest-last publication under `models/translategemma-4b-stage4/`.
- `tools/build-gemma.ps1 -Test` builds and executes the freestanding ARM64 C
   runner without Python or QNN. All 7470 reference cases pass, covering every
   added token, every supported language code, multilingual and randomized text,
   whitespace, byte fallback, malformed input, and context/output bounds. Eighteen
   artifact corruption checks pass, including malformed tables with recomputed hashes.
- The build automatically audits the PE: Kernel32 is its only DLL import, with
   no exception or CLR tables. Stage 2 and Stage 3 regressions pass, and the full
   published Stage 3 W4/W8 artifact inventory was rehashed successfully.
- This stage supplies the tokenizer library and test executable, not a working
   translator. Numerical model references and QNN transformer blocks remain the
   next work in Stages 5 and 6. Normal builds consume existing artifacts; Python
   is neither a build prerequisite nor an inference dependency.

## Stage 5: Minimal numerical reference

**Status: RoPE and scaled-residual contract resolved; W4 generation/quality
remains blocked by degraded output and a token-limit failure (2026-09-15).**

Build a development-time reference that isolates model correctness from QNN
integration. It may use Python/Transformers, but all fixtures consumed by the C
runtime are immutable and hashed.

Actions:

1. Record BF16 outputs for embedding, RMSNorm, RoPE, grouped-query attention,
   gated GELU MLP, one complete local layer, one complete global layer, final
   normalization, and vocabulary projection.
2. Record W8A16 and W4A16 simulated outputs using the exact deployment packing and
   rounding rules.
3. Record greedy token sequences for a small multilingual translation corpus,
   explicitly setting `do_sample=False` and honoring both stop-token IDs.
4. Add freestanding scalar checks for packing, dequantization, RoPE, mask creation,
   KV indexing, and argmax tie-breaking. Do not implement a production-speed CPU
   copy of the complete model.

Exit criteria:

- Primitive and layer fixtures identify whether differences originate in export,
  quantization, graph composition, or generation policy.
- Greedy BF16 output is reproducible for every acceptance sentence.

Implementation contract:

- `tools/translategemma-reference.py` is an offline reference module invoked by
   the existing exporter with `--numerical-reference`. It is not linked into or
   needed by a deployed executable. Native PyTorch is unavailable for the local
   Windows ARM64 Python 3.14 environment; NumPy 2.4.3 provides the native BLAS
   reference, with bounded row-wise weight conversion rather than a full FP32
   copy of the model.
- BF16 simulation reads original checkpoint weights, accumulates matrix products
   in FP32, and rounds operation outputs to BF16 using ties-to-even. W8/W4 simulation
   reads the actual Stage 3 packed weights and stored FP16 scales, accumulates in
   FP32, and rounds activation outputs to FP16. These are explicit numerical
   reference semantics, not claims of bit-identical PyTorch or HTP accumulation.
- Recorded traces include embedding scaling, input/Q/K RMSNorm, Q/K/V projections,
   split-half RoPE, visibility masks, attention scores and probabilities, grouped
   attention output, residuals, gated GELU, complete local layer 0 and global layer
   5, final RMSNorm, and the tied vocabulary projection. Sparse absolute positions
   0, 1023, and 1024 exercise the local-window boundary. Array shapes, byte types,
   semantic precision, model identity, and SHA-256 are recorded in the manifest.
- **RoPE contract, version 2:** the checkpoint's `rope_parameters` is
   authoritative: global theta 1000000 with linear factor 8, local theta 10000
   with factor 1. Divide global inverse frequencies by 8 before multiplying by
   absolute positions. Stock Transformers 5.17.0/PyTorch 2.14.0 confirms this
   interpretation, including positions 0, 1, 1023, 1024, and 2047. Stage 3's
   `metadata/rope` now agrees with execution. The earlier schema-1 reference
   reproduced Transformers 4.57.3 ignoring this field; its factor-1 artifacts
   remain historical diagnostics and are rejected by current verification.
- **Residual contract, version 2:** BF16 remains unscaled. W8/W4 store the
   residual stream as `hidden / 32` in FP16. Divide the embedding multiplier and
   post-attention/post-MLP RMSNorm gains by 32 *before* casting to FP16. Input,
   pre-MLP, and final RMSNorm use epsilon `1e-6 / 1024 = 9.765625e-10` and their
   original gains. Q/K RMSNorm, projections, attention, and KV remain unscaled.
   This preserves the real-arithmetic model, not identical finite-precision
   rounding. Do not cast an unscaled branch first, or change epsilon only after
   FP16 conversion. The vocabulary projection receives the final unscaled norm.
   Cross-variant residual-output errors are reported after multiplying W8/W4
   stored outputs by 32; logits are compared directly.
- The acceptance corpus includes the official model card's Czech-to-German
   example, English-to-Japanese, and German-to-English with a time expression.
   Messages use only a User role with exactly one text content entry containing
   `type`, `source_lang_code`, `target_lang_code`, and the text to translate.
   `apply_chat_template(..., add_generation_prompt=True)` supplies the Assistant
   prefix. Greedy decoding honors IDs 1 and 106 and decodes only newly generated
   tokens, as in the model card's direct-initialization example; image input and
   unsupported alternative prompting remain out of scope.
- `src/tools/gemma/gemma_numeric.h/.c` supplies only small allocation-free scalar
   primitives, not a production CPU transformer: FP16 conversion, signed W4/W8
   dequantization, split-half rotation, causal/local visibility, per-layer
   `[head,slot,channel]` KV element offsets, and first-maximum argmax. Invalid
   dimensions/positions/scales and nonfinite logits are rejected. Byte offsets
   for FP16 KV storage are twice the returned element offsets; local slots wrap
   at 1024 and global slots at 2048, with absolute positions retained for masking.
- `build-gemma.ps1 -TestNumerics` adds the independent hashed scalar fixtures to
   the existing C runner and PE audit. Ordinary builds/tests still need no Python,
   NumPy, Transformers, or QNN. `-ExportNumerics` explicitly regenerates the full
   offline corpus. `--primitives-only` exports a deliberately incomplete scalar
   set for fast checks and never labels Stage 5 complete.
- This three-sentence corpus is a numerical/generation regression gate, not the
   Stage 10 multilingual quality evaluation. W4/W8 changes are reported against
   BF16, not accepted by an invented quality threshold. The traces do not prove
   HTP accuracy or performance; those require the Stage 6 hardware comparison.
- A finite FP32 value outside FP16 range is a recorded numerical failure, never
   clipped or silently promoted to another deployment precision. Quantized
   attempts preserve the failing pre-cast FP32 activation, prompt IDs, layer and
   operation, and error. Such a case has no invented generated sequence. The
   manifest's `complete` flag denotes coverage of reference generation, while
   `quantized_generation_ready` separately requires every translation to finish.
   A quantized response exhausting 64 tokens is retained with its actual IDs,
   decoded bytes, residual maximum, and `status=token_limit`; readiness stays
   false. BF16 must still terminate and reproduce exactly. No token-budget
   extension or quality waiver is applied to repetitive output.
   **Root-cause confirmation:** stock upstream decoder layers independently
   reproduce the Czech prompt's unscaled layer-5 MLP residual overflow at W8
   73520 and W4 71568. BF16 stays finite. The optional x64 Windows oracle runs
   under emulation, isolated from native ARM64 calibration; it is development
   tooling only. Its stock Linear additionally rounds dequantized weights to
   FP16, so this is independent failure reproduction, not HTP bit parity.
   The full-depth BF16 prompt peak is 296224. Divisor 32 gives more than fourfold
   headroom against 65504; scaled W8/W4 prompt peaks are 9344.28125/8977.46875.
   All three prompts reach finite vocabulary logits. This measured corpus bound
   is not a proof for every possible 2048-token input.
- The optional `--residual-bound-audit` scans every mapped embedding row and
   bounds each post-norm channel by `sqrt(2560) * abs(1 + weight) / 32` across
   all 68 residual additions. Including FP16 relative rounding and subnormal
   allowances, the envelopes are W8 34446.264 and W4 34446.281, below 65504.
   This input-independent residual bound assumes finite branch inputs and
   exact-real RMSNorm; FP32/HTP normalization error and projection overflow
   remain outside its scope. The report is preserved under
   `models/translategemma-residual-bound-audit.json`.
- HTP core 2.39 / QAIRT 2.50 accepts FP32 add IO but does not preserve a residual
   sum of +/-73728. Wider IO types alone are therefore not a fix. The model-width
   `[3,2560]` FP16 add/RMSNorm probe passes with divisor 32, including a large
   residual, epsilon-sensitive small values, and zero. These are capability
   checks; Stage 6 must still compare actual folded norm gains and full blocks.
- The existing `test-translategemma-stage3.py` has optional `--torch-oracle` and
   `--residual-audit` modes. The former pins torch 2.14.0 and Transformers 5.17.0;
   the latter uses native NumPy and the existing tokenizer environment. Reports
   live in ignored `models/translategemma-precision-oracle.json` and
   `models/translategemma-residual-audit.json`. The x64 oracle report records
   the upstream module hash, checkpoint configuration hash, and exact prompt IDs.
- A separate `--residual-rounding-audit` compares W4's Japanese prompt with
   scaled FP16 versus diagnostic FP32 residual/branch outputs, keeping the same
   quantized projection weights. Both select first tokens `[220844,37307]`;
   the wide stream peaks at 287257.65625. Both tokens also match BF16: the
   Japanese divergence starts at token 3, so this two-token comparison does not
   rule out residual-rounding effects at the failing decision. The optional
   `--divergence-audit` replays each saved common prefix and records the first
   divergent decision's logits under BF16, W8, W4, wide-residual W4, and a W8
   vocabulary head applied to the same W4 hidden state. No production FP32/CPU
   fallback is introduced.
- The completed first-divergence replay reproduces all three saved decisions.
   The table reports logits of the BF16 choice minus the original W4 choice;
   a positive margin does not imply that either is the overall argmax.

   | Decision (generated token) | BF16 | W8 | W4 | Wide-residual W4 | W4 hidden, W8 head |
   | --- | ---: | ---: | ---: | ---: | ---: |
   | Czech (6): ` kann` versus ` bis` | 9.75 | 8.84375 | -5.703125 | -5.65625 | -4.890625 |
   | Japanese (3): full stop versus ` (` | 21.75 | 21.609375 | -4.90625 | -4.90625 | 0.65625 |
   | German (1): `The` versus opening quote | 18.25 | 18.125 | -0.578125 | -0.546875 | 12.40625 |

   Wider residual arithmetic repairs none of these choices. The W8 head selects
   the BF16 token for Japanese and German, but selects a comma for Czech, not
   ` kann`. This isolates an output-head contribution and leaves upstream
   transformer quantization damage in the Czech case; it does not establish a
   complete mixed-precision remedy. German's original quoted translation,
   "The train arrives at 3:30 PM.", is semantically valid despite differing IDs.
- Reciprocal `--embedding-divergence-audit` runs preserve the same saved prompts
   and common prefixes. W8 input embeddings with W4 layers/head repair none of
   the three choices (margins -6.625/-3.4375/-0.140625); W4 embeddings with W8
   layers/head preserve all three BF16 choices (6.84375/19.859375/14.40625).
   Input-embedding promotion alone is therefore not a supported fix. These are
   first-decision tests, not complete generation or multilingual quality gates.
   Reports live in ignored `models/translategemma-divergence-audit.json` and
   `models/translategemma-embedding-divergence-audit.json`.
- Full `--head-generation-audit` uses W4 input embeddings/layers and a W8
   output head with unchanged greedy decoding and the 64-token limit. All
   three outputs stop normally, but the change does not resolve quality:
   Czech still replaces the specific lens-cracking meaning with destruction.
   Japanese emits the correct greeting and full stop, followed by
   unwanted "(Ohayou gozaemas.)". German produces the valid "The train will
   arrive at 3:30 PM." None matches the complete BF16 token sequence. The
   ignored report is `models/translategemma-w8-head-generation-audit.json`;
   no mixed-precision deployment, stopping-rule change or quality waiver follows.
- The diagnostic-only `--mse-divergence-audit` candidate minimizes per-row
   reconstruction error over the original absmax quantizer and clipped scales
   from 0.95 to 0.5 times absmax. Candidates use stored FP16 scales and retain
   signed W4 values in [-7,7], without changing tensor layout. Lower weight
   error is not a translation-quality guarantee. `--mse-generation-audit`
   additionally generates the three complete responses using the same cached
   candidate weights. Neither mode changes the exporter or published artifacts.
- The completed row-MSE decision experiment does not repair the two degraded
   translation cases. Czech chooses a comma; the BF16 ` kann` token drops from
   rank 4 to rank 20, with a -2.8828125 margin against the original W4 ` bis`.
   Japanese still chooses ` (` over its full stop (margin -1.75). German selects
   `The` (margin 9.59375), but its original W4 translation was already valid.
   The same-format candidate is not promoted; reduced reconstruction error
   alone is insufficient evidence of useful model behavior.
- `--group32-generation-audit` tests the more specific hypothesis that a single
   scale across an entire input row (up to 10240 weights) is too coarse for W4.
   It uses independent absmax scales for contiguous groups of 32, signed packed
   W4 values, FP16 scales/activations, unchanged normalization and saved prompts.
   It records all three divergent decisions and then all three full generations
   using the same cached weights. This is an offline diagnostic, not a compatible
   Stage 3 artifact: current readers and QNN graphs require per-output-channel
   scaling. Any deployment would need an explicit format/graph change and new
   hardware validation. The candidate does not overwrite existing artifacts.
- Group-32's completed decision replay selects Japanese's full stop (margin
   19.375, rank 1) and German's `The` (25.59375, rank 1). Czech selects a comma;
   ` kann` remains rank 5, with margin -8.09375 against the original ` bis`.
   These are decisions at saved common prefixes, not full-generation acceptance.
   The manifest contains 3,879,895,040 matrix weights: packed W4 remains
   1,939,947,520 bytes, while group-32 FP16 scales require 242,493,440 bytes
   instead of 2,543,744. With unchanged norm tensors, total payload would be
   2,183,177,216 bytes, 239,949,696 bytes above the current W4 payload.
- The pinned QAIRT 2.50 headers expose block encodings, but HTP capability queries
   return `0x7d0` (`QNN_PROPERTY_ERROR_UNKNOWN_KEY`) for all seven queried
   encodings, including the working bit-width per-axis control. This query
   cannot establish support or non-support. The experimental Gemma probe now
   reports these raw statuses without changing its existing acceptance gates.
- An opt-in `GEMMA_GROUP32_DIAGNOSTIC` build of the capability probe tests
   `[1,2560] x [2560,2560]` with block dimensions `[32,1]`, four-bit signed
   values in an S8 container, and varying input-group/output-channel scales.
   The SDK's `BW_BLOCK_MAPPED` encoding is pointer-valued in the quantization
   union; the probe uses a function-lifetime encoding structure. HTP accepts
   creation/finalization/execution but fails the scalar reference at output 0
   (absolute error about 79.478512, unchanged tolerance 0.25). It is not an
   accepted hardware implementation. The failure log is retained under ignored
   `build/gemma-group32-capabilities.log`. Default builds omit this failing
   experiment; the established capability suite and all cleanup calls still
   pass. No production graph, shared ABI, Whisper source or published context
   is changed. The offline grouped candidate requires complete generation
   results and a numerically validated hardware encoding before deployment.

Validation record:

- The version-2 set publishes 184 verified artifacts. All three BF16 sequences
   replay exactly, and W8 matches all three BF16 token sequences byte for byte
   (18/4/15 tokens including stop). All nine attempts remain numerically finite.
   W4 terminates the Czech and German examples with changed text, but its
   Japanese case reaches the token limit: `complete=true` denotes diagnostic
   coverage, while `quantized_generation_ready=false` correctly remains set.
   Recorded residual maxima are BF16 296224, W8 9344.28125, W4 8977.46875.
   The final generation run took 3246 seconds; no timing claim for inference
   follows from this offline reference run. All artifact hashes/inventory,
   12 Python regressions, 323 C scalar cases, 7470 tokenizer cases, 18 corruption
   cases, and the Kernel32-only ARM64 PE audit pass after publication.
- Version 2 eliminates the observed residual overflow without clipping or a
   CPU fallback. W4 still gives degraded Czech-to-German wording and repeats a
   romanized greeting in the Japanese case until the 64-token limit. The actual
   response is retained as a failed generation diagnostic, not a translation
   acceptance. Continue Stage 6 with W8 first; investigate W4 quantization
   quality separately rather than changing RoPE back or relaxing stop criteria.
- The historical ignored set under `models/translategemma-4b-stage5/` contains
   184 validated artifacts, including all three variants' primitive and complete
   local/global layer traces, BF16 prompt/generated-token sequences, and six
   quantified FP16 overflow diagnostics. `quantized_generation_ready=false` is
   intentional: no W8/W4 full-model token sequences are claimed.
- In that historical set, all three BF16 translations reproduced exactly on a second full cached run.
   The manifest stores the exact Czech-to-German, English-to-Japanese, and
   German-to-English translation bytes, hashes, and 18/4/15-token sequences
   respectively, including each end-of-turn token. The time example produces
   "The train is scheduled to arrive at 3:30 PM."
- The freestanding C runner passes 323 numerical cases, including all 65536
   FP16 bit patterns, plus the existing 7470 tokenizer and 18 corruption cases.
   ARM64/Kernel32-only imports and empty exception/CLR tables pass the PE audit.
- Stage 2's 12 tests passed at initial publication; the expanded Stage 3/reference
   suite's 12 tests pass after the contract correction and token-limit handling.
   Independent checks cover RMSNorm, closed-form grouped attention, sparse
   local/global mask boundaries, cached-versus-full attention, actual W4/W8
   payload mappings, diagnostic preservation, and malformed array metadata.
- Source and all Stage 3 W4/W8 payload hashes were revalidated before generation;
   every numerical artifact and the complete output inventory pass read-only
   verification at initial publication. New artifacts use schema 2 under
   `models/translategemma-4b-stage5-v2/`; both Python verification and the
   Python-free C build entry point reject old or conflicting execution contracts.
   Neither diagnostic coverage nor finite generation is Stage 10 quality acceptance.

## Stage 6: QNN transformer block

Prove one local and one global decoder block before constructing all 34 layers.

Status: implemented and hardware-validated on 2026-09-15, Snapdragon X Elite,
QAIRT 2.50 / QNN core 2.39. Both W8A16 and W4A16 pass local layer 0 and global
layer 5 block gates. This is block arithmetic acceptance, not W4 translation
quality acceptance or a full-depth prompt/decode implementation.

Actions:

1. Build W8A16 graphs first and compare all meaningful intermediate outputs.
2. Replace projections with W4A16 while retaining the version-2 FP16 residual
   stream (`hidden / 32`), compensated normalization, attention, and KV storage.
3. Fold RoPE into Q/K preparation and use four KV heads with eight query heads.
4. Supply a fixed-size causal mask and a runtime position input. Avoid rebuilding
   or finalizing graphs per position.
5. Bind KV storage through registered FastRPC shared memory and verify offset,
   alignment, lifetime, and cache visibility explicitly.
6. Measure graph creation, finalization, first execution, warmed execution, DDR
   traffic when available, and numerical error.

Exit criteria:

- Local and global blocks pass their W4A16 numerical gates.
- Repeated execution updates or consumes the intended KV rows without copying a
  complete cache through ordinary application buffers.
- The warmed block result is deterministic and all failure paths release handles
  in reverse order.

Implementation and reproducible gate:

```powershell
.\experimental\snapdragon\tools\build-gemma.ps1 -TestBlocks -TestNumerics
```

The tool-private `src/tools/gemma/gemma_block.h/.c` builds a fixed three-token
graph with runtime positions and masks, in-graph split-half RoPE, four KV heads,
eight query heads, 2048 past slots, and three current slots. It retains the v2
scaled residual contract. W8 needs QNN's standard axis-scale-offset encoding;
the bit-width encoding is rejected for W8 on this backend. W4 uses the proven
S8 container with bit width 4 and output-channel scales. Stored matrix metadata
declares quantization axis 1; both variants are transposed for QNN MatMul.

The dedicated Kernel32-only ARM64 runner validates artifact identity and SHA-256,
then creates/finalizes once per block. It registers four page-aligned regions in
one FastRPC allocation: past K, past V, current K, and current V. Guard padding
and read-only cache fingerprints are checked after every execution. Only the new
three-token rows are copied inside shared storage, 12288 bytes per step; no
complete cache travels through ordinary application buffers. Local slots wrap
modulo 1024; global slots use absolute positions below 2048. Concat/head expansion
still materializes tensors inside HTP; this is not a zero-DDR production graph.

The offline fixture set contains 209 hashed artifacts, generated through
`export-translategemma.py --block-reference` into
`models/translategemma-4b-stage6/`. It uses actual W8/W4 weights and the existing
NumPy 2.4.3 reference, without full-model generation. Both layers receive the
same three embedded token IDs `[2, 9259, 1902]`, at positions `[0, 1023, 1024]`
then `[1025, 1026, 2047]` with cached K/V. Layer 5 is intentionally tested in
isolation, not with layer 4's output. `complete=false` describes the absence of
the full Stage 5 corpus in this block-only publication, not a failed block gate.

Twenty meaningful taps per step are compared, including visible attention scores
mapped from physical cache slots and softmax probabilities. All masked
probabilities must be zero. The fixed gate requires
`sum(error^2) <= 0.000625 * sum(reference^2) + 0.000001 * element_count`
and `max_abs_error <= 0.15 * max_abs_reference + 0.02`, plus finite outputs.
Thus the relative RMSE budget is 2.5% with a 0.001 absolute RMS floor. All observed
tap relative MSEs were below 19 ppm (relative RMSE below 0.44%); final block
outputs were below 3 ppm. Folded FP16 norm gains and backend GELU/softmax arithmetic
are tolerance-checked against the FP32-accumulating, explicitly rounded oracle,
not asserted to be kernel-bit-identical.

Each step has three warm replays with matching fingerprints for every tapped
output. Masking out past rows must change attention; restoring the mask must
reproduce it. Five injected failures after context creation, graph construction,
finalization, partial registration, and execution all return failure with zero
cleanup errors. Registered handles are released before their backing allocation;
the context is freed before weight/tensor storage, then device/backend/log and
loaded modules are released. FastRPC's free API itself has no status return.

Measured final gate (construction includes artifact reads, hashing and transpose;
warm median covers six replays, no power-mode control):

| Block | Construction s | Finalize s | First ms | Warm Median ms |
|---|---:|---:|---:|---:|
| W8 local 0 | 1.181 | 2.940 | 16.49 | 11.30 |
| W8 global 5 | 1.031 | 2.802 | 16.58 | 12.11 |
| W4 local 0 | 0.820 | 2.629 | 14.09 | 10.19 |
| W4 global 5 | 0.839 | 2.609 | 14.52 | 10.29 |

Logs and binding files are under `build/gemma-block/`; the runner has 74 graph
tensors with diagnostic outputs retained. DDR traffic was not measured because
no hardware traffic counter was requested. These figures do not establish the
Stage 7/8 performance targets. Full/deep caches, all-layer composition, prompt
padding, serialized restore, and full-model HTP generation remain later gates.
The production Whisper executable/context and historical reference sets are
unchanged. The build also passes tokenizer 7470, corruption 18, numeric scalar
323, and ARM64 no-CRT PE audits.

## Stage 7: Prompt processor

Create a fixed-shape prompt path that amortizes weight traffic across many input
tokens.

Status (2026-09-16): Stage 7 complete. All three context buckets pass the
253/256-token prompt gates, fresh-process restore, corruption rejection,
and the unchanged 300 input tokens/s requirement.
The shape-aware block builder supports 128-token chunks and 512/1024/2048 buckets,
unique layer names, internal hidden connections, and final-token vocabulary
projection. The existing no-CRT runner now composes all 34 W4 layers into one
graph. Each finalized context is serialized with an application envelope;
fresh-process QNN restoration takes about 1.4 seconds, excluding file IO and
envelope hash verification. Construction-only weight buffers are released before
restore; no BF16/W8 artifact variant is loaded for this path.

The original DMA failure (result 1100, transport error 0) reproduces with only
two blocks using separate runtime position tensors. Sharing one position input
across all layers lets the complete restored graph execute. Independent
128-token blocks and the full vocabulary projection also execute, so neither
graph size nor the vocabulary projection explains that minimal failure.
Local/global cosine, sine, and mask inputs are shared within their attention
class; borrowed descriptors are not rebound or serialized twice.

Execution then exposed a separate position-lookup error: direct INT32 and UINT32
application inputs selected the next cosine/sine row in the 128-token probe,
producing about 24870 ppm relative MSE. Changing index rank or supplying unit
quantization metadata did not help. FP32 application inputs followed by an
explicit graph Cast to INT32 return the correct rows. All supported positions
are exactly representable in FP32. The final-token selector uses the same
conversion. The original fused RotaryEmbedding is retained: both fused and
explicit arithmetic pass with corrected inputs, so the fused operator was not
the numerical cause. The exact backend-internal conversion defect is unknown.

The prompt gate first runs two independent blocks sharing only positions,
checks equal finite outputs at positions 0..127 and 128..255, and compares
rotated keys against split-half FP16 arithmetic using actual normalized keys.
Both primitive comparisons report below 1 ppm relative MSE; cleanup passes.

Run all graph and fresh-process restore gates:

```powershell
foreach ($bucket in 512, 1024, 2048) {
   .\experimental\snapdragon\tools\build-gemma.ps1 -TestPrompt -PromptBucket $bucket
   .\experimental\snapdragon\tools\build-gemma.ps1 -TestPrompt -RestorePrompt -PromptBucket $bucket
}
```

The first command builds and stores `build/gemma-block/prompt-<bucket>.gmb.context`;
the second requires that bucket's context and avoids rebuilding/finalizing the graph.
Envelope version 4 rejects older integer-input contexts. The envelope retains
graph/tensor names and IDs, dimensions, model repository
and pinned revision, W4/chunk/bucket metadata, QNN core/backend versions, and
SHA-256 covering metadata and binary. Before deserialization, the runner checks
bounded identity strings, version and bucket compatibility, exact file size,
and all 146 expected IO descriptors (names, owners, types, dimensions, and
unique IDs). Invalid shapes cannot reach allocation-size arithmetic.
The envelope regression rejects 75 malformed-header or damaged-hash/payload
cases across the three bucket schemas, including freshly rehashed invalid
metadata. The script also checks actual empty files, truncated headers, and
truncated payloads through the restore path, requiring rejection before QNN
deserialization and zero cleanup errors. These integrity checks detect
corruption; SHA-256 is not authentication of an untrusted model publisher.

Offline `export-translategemma.py --prompt-reference` publishes 77 verified
artifacts under `models/translategemma-4b-stage7/`: full-depth W4 K/V for a
256-token deterministic sequence, embeddings, local/global RoPE tables, and
logits at tokens 253 and 256. Earlier positions of the full causal run provide
the unpadded 253-token reference. The runner's registered K/V output path,
253-token padded comparison, all-layer retained-row/logit checks, deterministic
256-token repeats, and 300 tokens/s gate all pass for every bucket. Every
retained KV row in all 34 layers passes the unchanged Stage 6 tolerances for
both 253 and 256 valid tokens. Fresh-process replay reports key relative MSE
up to 52 ppm, value relative MSE up to 116 ppm, and logit relative MSE of 53 ppm
(253 tokens) and 22 ppm (256 tokens). Guard checks, three deterministic warm
replays, and cleanup pass. Final fresh-process results:

| Context Bucket | QNN Restore | Warm Input Tokens/s |
| --- | --- | --- |
| 512 | 1.398 s | 488 |
| 1024 | 1.477 s | 449 |
| 2048 | 1.382 s | 353 |

The initial 2048 run was numerically correct but reached only 283 tokens/s.
The host was rebuilding the same shared masks once per layer. Filling positions
once and local/global masks once each per chunk removed the redundant work;
the graph, serialized context, timing boundary, and acceptance tolerances did
not change. These are runner wall-clock measurements (including input/control
preparation and KV copies), not DDR traffic measurements or a power-controlled
benchmark. All 75 in-memory corruption cases and nine file-rejection cases pass.

The planned Stage 7 exit criteria pass. Coverage uses the deterministic 253/256
token fixture in each bucket; it is not a full-length 2048-token reference sweep
or an end-to-end translation-quality test. The independent Stage 5 W4 quality
gate remains blocked. Stage 8 adds single-token generation and persistent KV state.

Existing Stage 6 local/global W8/W4 hardware checks, five failure-cleanup probes,
7470 tokenizer cases, 18 corruption cases, 323 numerical scalars, and no-CRT PE
audits still pass after the builder changes. Twelve Python reference tests pass.

Actions:

1. Begin with 128-token chunks and context buckets of 512, 1024, and 2048 tokens.
2. Compose all 34 layers into one graph where QNN finalization permits it. If the
   graph compiler requires partitioning, use the smallest measured partition
   count and never default to one submission per layer.
3. Produce packed K/V rows directly into registered shared memory.
4. Mask padding so the final short chunk is numerically identical to an unpadded
   reference.
5. Serialize the finalized prompt context with an application header containing
   graph names, tensor IDs, model identity, shape bucket, QNN version, and hash.

Exit criteria:

- Prompt logits and every retained KV row pass reference tolerances.
- A 256-token prompt reaches at least 300 input tokens/s after warm restore.
- Prompt processing does not retain an unnecessary BF16 or W8 copy of W4 weights.

## Stage 8: Token generator and persistent KV state

The token generator is the decisive performance stage. It must execute one token
with one QNN submission, or with a very small fixed number justified by a faster
end-to-end result.

Actions:

1. Build a one-token graph containing all 34 layers, final RMSNorm, and the tied
   vocabulary projection.
2. Keep local and global KV caches in registered shared memory for the complete
   request. The graph consumes prior rows and emits the current packed K/V rows;
   a bounded host copy of only those new rows is acceptable if in-graph state
   update is unavailable.
3. Pass position, RoPE values, and masks as small inputs. Do not build one graph
   per token position.
4. Prefer a verified in-graph ArgMax for greedy decoding. Returning 262208 FP16
   logits for host argmax remains a correctness fallback and costs only about
   512 KiB per generated token.
5. Evaluate separate prompt and decode contexts to avoid simultaneous duplicated
   weights on the 16 GiB machine. Measure context-switch cost before deciding
   whether both can remain resident.
6. Serialize and restore the final graph; production execution must never rebuild
   nodes from model weights.

Exit criteria:

- Generated token IDs match the W4A16 reference.
- Warm decoding reaches at least 10 tokens/s at 512- and 2048-token contexts;
  15 tokens/s is the target.
- No per-layer host dispatch and no complete KV-cache copy occurs per token.
- No individual QNN call stalls above the documented acceptance threshold in a
  sustained translation run.

## Stage 9: End-to-end translation tool

Add a dedicated executable rather than adding Gemma modes to `npu_probe.exe`.

Proposed interface:

```text
translategemma.exe --from=cs --to=de-DE [--max-tokens=256] [--quiet] < input.txt
```

Actions:

1. Keep stdin/stdout UTF-8 and provide file arguments only if needed by measured
   workflows.
2. Restore QNN contexts once and process multiple requests in one process.
3. Implement deterministic greedy generation with EOS and maximum-token bounds.
4. Report model/context identity, token counts, prompt and decode rates, time to
   first token, peak memory, and QNN call statistics outside quiet mode.
5. Stream decoded output without emitting invalid or incomplete UTF-8 sequences.
6. Use distinct builder and runtime executables if graph construction materially
   increases production code or data size, following the Whisper precedent.

Exit criteria:

- A clean process translates every acceptance sentence and exits without leaked
  QNN, file, memory, or thread resources.
- Quiet output contains only translated UTF-8 text.
- The runtime binary has no CRT, exception, or CLR tables and statically imports
  only `KERNEL32.dll`.

## Stage 10: Quality, performance, and memory gates

Use a fixed multilingual corpus with short sentences, long paragraphs, difficult
scripts, named entities, numbers, markup-like text, and near-context-limit input.

Required gates:

| Metric | Minimum gate | Target |
| --- | ---: | ---: |
| Warm decode, 512-token context | 10 tokens/s | 15+ tokens/s |
| Warm decode, 2048-token context | 10 tokens/s | 15+ tokens/s |
| 256-token prompt prefill | 300 tokens/s | 500+ tokens/s |
| 256-token prompt TTFT | under 2.0 s | under 1.0 s |
| Peak working set | under 11 GiB | under 8 GiB |
| Static PE imports | Kernel32 only | Kernel32 only |
| Malformed UTF-8 output | zero | zero |

Quality gates:

- Compare W4A16 against the original BF16 model, not only against a community
  quantization.
- Record exact greedy token agreement for deterministic fixtures.
- Measure COMET, chrF, or another pinned translation metric on a representative
  subset of the 55 evaluation languages.
- Set the acceptable W4 quality delta only after recording W8 and BF16 baselines;
  do not select clipping or group size from a single language pair.
- Review every changed translation in the small fixed regression corpus.

### Quality Evaluation Workflow

`tools/translategemma-quality.json` is a versioned pilot corpus: 24 diagnostic
cases and 24 reserved held-out cases. The original three numerical regression
fixtures remain unchanged. Coverage includes Japanese greetings/questions and
reverse translation, explicitly optical Czech context, negation and role/order
minimal pairs, numbers/dates/units, names, quotations, markup and short paragraphs.
These assistant-authored examples and references have AI semantic approval by
GitHub Copilot, as authorized by the user on 2026-09-16. This is author self-review,
not independent human review. They are not a representative public benchmark, 55-language
coverage, or a near-context-limit stress corpus.

Split rules:

- Calibration data is separate and is not supplied by this corpus. Neither
   split may be added to quantizer calibration data.
- Use only `diagnostic` for development and quantizer selection. The validator
   rejects duplicate source text (case/whitespace/NFC normalized) across splits.
   Reviewers must also check paraphrase and translation leakage.
- Review held-out references and semantic rubrics before model evaluation.
   Record `review.heldout.status=approved`, an actual reviewer identity in
   `reviewer`, `reviewer_kind=ai` or `human`, and the `cases_sha256` printed by
   `validate` after review. AI approval is sufficient for this pilot; human review
   is not a required blocker. Changed
   cases invalidate that hash. Do not mark assistant-generated references as
   human-reviewed. An approval record is an audit trail, not authentication.
- Freeze quantizer choices, decoding budget and comparison policy before the
   held-out run. It requires all three baseline variants and forbids case filters.
   Once results inform tuning, that split is development data; reserve a fresh
   independently reviewed version for subsequent acceptance.

Offline commands from the repository root (use a new output path for each run):

```powershell
$python = './experimental/snapdragon/build/calibration-venv/Scripts/python.exe'
$quality = 'experimental/snapdragon/tools/translategemma-quality.py'
& $python -m pip install -r experimental/snapdragon/tools/calibration-requirements.txt
& $python -B $quality validate
& $python -B $quality prepare --output experimental/snapdragon/models/quality-prompts-v1.json
$env:OPENBLAS_NUM_THREADS = '4'
& $python -B $quality run --output experimental/snapdragon/models/quality-diagnostic-v1.json
& $python -B $quality score --report experimental/snapdragon/models/quality-diagnostic-v1.json
# Run after the baseline, not concurrently on a 16 GB machine:
& $python -B $quality run --variants candidate --candidate-base group32 --output experimental/snapdragon/models/quality-group32-v1.json
# Complete the diagnostic candidate's AI/human review packet before freezing:
& $python -B $quality freeze --report experimental/snapdragon/models/quality-group32-v1.json --reviews experimental/snapdragon/models/quality-group32-v1.json.reviews.json --output experimental/snapdragon/models/quality-selection-v1.json
& $python -B $quality run --split heldout --variants bf16 w8a16 w4a16 candidate --candidate-base group32 --frozen experimental/snapdragon/models/quality-selection-v1.json --output experimental/snapdragon/models/quality-heldout-v1.json
# Complete the held-out review packet before evaluating:
& $python -B $quality evaluate --report experimental/snapdragon/models/quality-heldout-v1.json --reviews experimental/snapdragon/models/quality-heldout-v1.json.reviews.json --frozen experimental/snapdragon/models/quality-selection-v1.json
```

The default run evaluates 24 prompts serially in BF16, W8A16 and the published
W4A16 format. `--variants candidate --candidate-base group32` evaluates group-32
offline without exporting deployment weights. `--candidate-base w4a16` retains
the published row quantizer. Repeated `--w8-layer N` promotes selected decoder
layers (0..33); `--w8-head` promotes only the vocabulary projection, retaining
the base input embedding. Candidate reports record configuration, implementation
hash and logical packed payload bytes, including the extra W8 head when selected.
These are diagnostic substitutions, not full-model hardware acceptance.
`--case d01` and
`--variants w8a16` support diagnostic smoke tests. The new corpus has a fixed
default 128-new-token budget (configurable before evaluation, never extended
after observing repetition); the legacy three-case gate remains at 64. The
runner validates prompt plus output budget against 2048 tokens. Token IDs and
hashes of the corpus, tokenizer/configuration, model configuration, weight
manifest and numerical reference source are saved. Partial runs are marked
incomplete, existing report paths are rejected, and scoring refuses incomplete
or duplicate/missing results. Writes are atomic; `run --resume <checkpoint>`
requires a new output path and identical case prompts, provenance, variants and
decoding settings, validates saved result health and review IDs, and skips only
completed cases. Do not resume a checkpoint that another process is still writing.
Token-limit and repeated-four-gram flags are
diagnostics, not stopping-rule changes; repetition flags require semantic review.

Every completed run writes a `.reviews.json` packet without variant labels,
ordered by opaque review ID. A human or AI reviewer rates `meaning`, `omission`,
`hallucination`, `terminology` and `unwanted_content` as `correct` (no error),
`minor` (usable despite a small error), or `major` (meaning changed or unusable),
with notes explaining the judgment. Valid paraphrases are correct; changes of
object, negation, roles, material numbers, or uncontrolled repetition are major
when they defeat the source meaning or requested translation. Ambiguous cases
must be flagged for discussion rather than forced into a confident verdict.
BF16 is a baseline, not a gold answer. Fill the packet's reviewer identity and
`reviewer_kind` (`ai` or `human`) and score with
`--reviews <packet-path>`. Its report hash and complete review-ID inventory must
match. Reports distinguish `semantic_review_complete` from
`human_review_complete`; AI reviews never set the latter true. The packet is
for ordinary blind review, not adversarial concealment.

Scoring uses `sacrebleu==2.5.1` chrF++ (character order 6, word order 2, beta 2),
records its signature, and reports corpus scores, per-language-pair mean sentence
scores, health counts, exact BF16 token agreement, and attributed review error counts.
Compare paired cases and per-language results, not only an aggregate. No metric
threshold or automatic quality acceptance is inferred from chrF++;
`quality_accepted` remains false. The separate predeclared pilot semantic gate
requires all 24 cases, zero major-error cases, at most four minor-only cases, and
termination without empty or replacement-character output. Valid paraphrases are
correct. A candidate must pass this gate on reviewed diagnostic outputs before
`freeze` creates the selection record. This conservative pilot rule is an
engineering screen, not a statistically established general-quality threshold.
`freeze` binds candidate settings, token budget, held-out cases, model/tokenizer/
implementation hashes and diagnostic report/review hashes. Held-out runs require
that exact selection plus all three baselines. `evaluate` reports
`pilot_quality_accepted` separately from `deployment_accepted=false`; the latter
still requires hardware and full-model gates. The held-out split becomes spent
once used for selection or tuning, regardless of a pilot pass/fail result.
Reference approval only approves
the test material, not the generated translations. New reports retain the AI
reference-review provenance; historical reports keep their original status.

Validation on 2026-09-16: 18 Python tests pass, including split/review/hash locks,
duplicate rejection, generation failure cleanup, report packets and metric
scoring. All 24 diagnostic prompts prepared successfully. A real W8 `d01` smoke
run produced the evening greeting and stopped with IDs `[197339,236924,106]`;
chrF++ is 100 against its draft reference, without a quality-acceptance claim.
The full 72-generation diagnostic run and held-out evaluation have not been run.

Campaign stopped by user request on 2026-09-16 at approximately 12:58 local time:
completion within another 30 minutes was not realistic. All 24 BF16 cases and
W8 cases d01..d08 (32 completed outputs) remain in
`models/translategemma-quality-diagnostic-v2.json` and the separate snapshot
`models/translategemma-quality-stopped-20260916-125804.json`. Both are explicitly
incomplete. No quality workers remain; the queued group-32 run did not start.
Earlier reports and checkpoints are retained. Do not restart this campaign or
its queued work without a new user request. No candidate has been selected or frozen, and
held-out generation remains locked pending completed diagnostic review. Twenty
Python tests pass, including mixed-precision routing, group reconstruction,
resume/atomic-write failure handling and held-out freeze/policy checks.

### Hardware Diagnostic Cases

Follow-up hardware investigation on 2026-09-16: explicit `Dequantize` before
MatMul and mapped blockwise expansion (encoding 10, channel-first group
multipliers) both reproduce 39/48 failures with raw-integer-like outputs.
The legacy inline block encoding (4) with S8 storage crashes with Windows
integer-divide exception `0xc0000094`; it is a negative control, not a supported
W4 path. ABI layouts were checked against the pinned SDK. Active HTP backend,
prepare, V73 stub and V73 skeleton hashes match the standard SDK archive files.
This narrows the failure but does not establish a general claim about all HTP
grouped formats or explain the backend's internal cause.

An explicit composition works: represent each 32-input group using the proven
per-axis W4 encoding, compute a partial MatMul, then sum FP16 partial outputs.
Both the tiny 48-output fixture and the `[1,2560] x [2560,2560]` projection pass
the existing scalar gates, along with the rest of Stage 1. The large control uses
80 MatMuls, 79 Adds and 80 host input slices. This establishes a projection-level
alternative, not efficient full-model execution; extra FP16 partial rounding,
graph size, memory and throughput need full-model validation before adoption.
The offline group-32 oracle still performs a single FP32 accumulation per
projection and must not be described as bit-exact to the composed HTP graph.

Use `build.ps1 -GemmaGroup32Diagnostic -GemmaGroup32Encoding composed` for this
isolated builder; modes `mapped` (default), `dequantize`, `expansion` and
`legacy-block` retain negative reproducers. The last three vary the tiny probe;
their subsequent large probe, if reached, is still mapped. Every opted-in
grouped gate now fails closed. Separate binaries/logs under `build/` preserve
the production Whisper binary and contexts. Logs are
`gemma-group-{dequantize,expansion,legacy,composition}.log`.

The existing Gemma probe now runs a tiny `[12,64] x [64,4]` per-axis control before
the model-shaped projection. Eight positive basis inputs select columns
0/1/30/31/32/33/62/63; two negative inputs select 31/32; the remaining inputs test
within-group cancellation and cross-group addition. Signed weights alternate by
input column and output channel; scales differ by channel and by group. All
expected values are exactly representable. Outputs start as NaNs; nonfinite
outputs fail explicitly in both tiny and model-shaped projection checks.

`build.ps1 -GemmaGroup32Diagnostic` enables the grouped tiny cases before the
large grouped projection and defaults to `build/gemma-group32-probe/`. It rejects
the production output directory and conflicting Whisper candidate switches.
Stage the pinned QNN runtime using the existing fetch script into that directory,
then run its `npu_probe_builder.exe --gemma-stage1`. Normal builds omit grouped
experiments; production Whisper source and contexts are unchanged. Script
execution remains subject to the user's PowerShell policy; this session used
the equivalent direct Clang commands, and separately parsed the build script.

Measured: all 48 per-axis outputs are exact and normal capability/cleanup gates
pass. The opt-in `BW_BLOCK_MAPPED` group-32 path fails 39/48 outputs (exit 126).
Basis values match raw signed integers, not dequantized weights: for example,
column 0/output 0 is 1 instead of 0.125, and the cross-group sum is zero instead
of 0.875. Cancellation alone passes and therefore cannot validate scaling. This
localizes the observed behavior to unapplied scales in the attempted encoding
path, not FP16 accumulation noise; it does not establish backend-wide support or
identify a deployment fix. Logs are retained as `build/gemma-basis-axis.log` and
`build/gemma-basis-group32.log`. Do not suppress exit 126 in acceptance jobs.

The earlier full group-32 three-case run is now complete: Japanese matches BF16
and stops, German is the valid paraphrase "The train is due to arrive at 3:30 PM.",
and Czech remains "Im schlimmsten Fall, bis zum Platzen der Scheibe." (wrong
object). This reinforces the need for the separate broader evaluation.

Benchmark reporting:

- Separate cold process, runtime load, context restore, tokenization, prompt
  prefill, first token, warm decode, detokenization, and cleanup.
- Report median, p95, maximum, and calls over 10/100/1000 ms.
- Report process CPU time, peak/current working set, private committed bytes, QNN
  host-call duty, generated tokens, and exact output hash.
- Use repeated interleaved runs and record Windows power mode because the existing
  Whisper measurements show substantial scheduling and power-state variance.

## Stage 11: Packaging and hardening

Actions:

1. Define a standalone directory layout containing the executable, pinned QNN
   runtime, model artifacts, context binaries, manifest, and required licenses.
2. Resolve resources relative to the executable or an explicit `--model-dir`;
   do not depend on the repository working directory.
3. Add deterministic tests for malformed CLI input, unsupported languages,
   missing runtime, provider/API mismatch, wrong context, damaged artifacts,
   allocation failures, QNN execution failures, output overflow, and cleanup.
4. Add an automated LLVM PE audit to the test runner.
5. Record model and QNN redistribution obligations. Quantized model artifacts
   remain governed by the Gemma terms.
6. Document exact clean-room reproduction from accepted model access through
   export, context build, tests, and deployment staging.

Exit criteria:

- The packaged directory runs outside the repository checkout.
- No Python, PowerShell, ONNX Runtime, Genie, Visual C++ runtime, or model-converter
  component is needed for inference.
- A fresh artifact corruption and QNN mock run proves all documented failures.

## Stop and reconsider conditions

Stop full-model implementation and revisit the design if any of these occur:

- The target HTP runtime cannot execute a numerically acceptable W4A16 projection.
- Prompt and token graphs require per-layer host submissions.
- Restored contexts require simultaneous weight copies that push the process over
  the 11 GiB hard memory gate.
- Mutable or host-updated shared KV state is unreliable across repeated graph
  execution.
- W4A16 translation quality misses the eventual multilingual acceptance bound and
  W8A16 cannot meet memory or speed gates.
- Sustained HTP calls reproduce multi-second stalls that cannot be eliminated by
  graph fusion, partitioning, or runtime upgrade.

If W4A16 quality fails but W8A16 fits, ship W8A16 only if it still exceeds the
10-token/s minimum. If direct QNN graph construction cannot meet the dispatch or
state requirements, retain the freestanding tokenizer and artifact work but do
not hide a Genie or ONNX Runtime dependency behind the production interface.

## Validation commands

Stage 0 retains the existing commands:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\build.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\test-npu-probe.ps1
llvm-readobj --file-headers --coff-imports .\experimental\snapdragon\build\npu_probe.exe
```

Stage 4 builds and runs entirely in C after artifact export:

```powershell
.\experimental\snapdragon\tools\build-gemma.ps1 -Test
```

Regenerate the pinned tokenizer tables and reference corpus only when needed:

```powershell
.\experimental\snapdragon\tools\build-gemma.ps1 -ExportReference -Test
```

See [README.md](README.md#translategemma-development) for the optional reference
environment setup. Later stages should retain the same build/runtime separation;
the following translation interface is not implemented yet:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\build-gemma.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\test-gemma.ps1
.\experimental\snapdragon\build\translategemma.exe --from=cs --to=de-DE < input.txt
```

## References

- [TranslateGemma collection](https://huggingface.co/collections/google/translategemma)
- [TranslateGemma 4B model card](https://huggingface.co/google/translategemma-4b-it)
- [TranslateGemma technical report](https://arxiv.org/abs/2601.09012)
- [Gemma 3 technical report](https://arxiv.org/abs/2503.19786)
- [Qualcomm AI Hub Qwen3 4B](https://aihub.qualcomm.com/models/qwen3_4b)
- [Existing Snapdragon QNN findings](snapdragon-x-qnn.md)
- [Existing Whisper benchmark record](benchmark.md)