# Snapdragon experiments

This directory documents freestanding Windows ARM64 experiments for the Snapdragon X Elite. Probe source and import definitions live under `src/`, scripts under `tools/`, and downloaded or generated model assets under the ignored `models/` directory. The native probe uses no C runtime, SDK headers, or bundled runtime libraries.

For Medium execution events, CPU accounting, latency distributions, and timeline
capture, see [diagnostics.md](diagnostics.md).

## GLM-OCR development

GLM-OCR is the third independent freestanding C/QNN experiment, alongside Whisper
and TranslateGemma. The pinned original checkpoint is staged under
`models/glm-ocr/`; the native ARM64 artifact verifier is built under `build/ocr/`.
It uses no CRT and imports Kernel32 only. Run the offline regression and full
model verification from the repository root:

```powershell
.\experimental\snapdragon\tools\build-ocr.ps1 -Test -Verify
```

Use `-Download` for initial resumable acquisition. Stage 1 verifies original
files and tensor geometry. Stage 2 adds a native Byte-Level-BPE tokenizer,
decoding and single-image task prompts, verified against 180,712 reference cases:

```powershell
.\experimental\snapdragon\tools\build-ocr.ps1 -TestTokenizer -Test
```

This consumes offline-generated `models/glm-ocr-tokenizer-v2/` artifacts without
Python in the native build/test path. It does not yet perform OCR or execute on the NPU.
See [plan-glm-ocr.md](plan-glm-ocr.md) for source identity, current limitations,
licensing provenance and the staged tokenizer/vision/decoder/QNN roadmap.

## TranslateGemma development

TranslateGemma currently has pinned W4/W8 weight artifacts and a freestanding C
tokenizer/prompt implementation. It does not yet execute the translation model.
See [plan-translategemma.md](plan-translategemma.md) for the stage record and
the Stage 5 numerical reference's explicit precision and RoPE contracts.

With the exported Stage 4 artifacts present, the normal build and regression gate
uses Clang/LLVM and one PowerShell entry point, with no Python or QNN dependency:

```powershell
.\experimental\snapdragon\tools\build-gemma.ps1 -Test
```

If PowerShell blocks repository scripts, first run
`Set-ExecutionPolicy -Scope Process Bypass` in that terminal. This changes only
the current process policy, not the persistent machine or user policy.

This produces `build/test-gemma-tokenizer.exe`. Its C tests consume immutable,
hashed `tokenizer.gta` and `tokenizer-fixtures.gta` files from
`models/translategemma-4b-stage4/`. The build audits ARM64 machine type, Kernel32-only
imports, and empty exception/CLR directories. No model weights or tokenizer data
are embedded in the executable. The existing Whisper executable is not rebuilt
or replaced.

Only table/reference regeneration needs a development Python environment. Reuse
the existing ignored calibration environment, creating it if absent:

```powershell
python -m venv experimental/snapdragon/build/calibration-venv
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe -m pip install numpy==2.4.3 transformers==4.57.3 tokenizers==0.22.2 jinja2==3.1.6
.\experimental\snapdragon\tools\build-gemma.ps1 -ExportReference -Test
```

NumPy is needed by the existing weight converter and Stage 3 tests, not the
tokenizer. Table-only export from the authenticated, hash-pinned Stage 2 download
works with Python's standard library alone:

```powershell
python experimental/snapdragon/tools/export-translategemma.py --tokenizer-only --output experimental/snapdragon/models/translategemma-4b-tokenizer
```

Use `--replace` explicitly to replace an existing export directory. Reference
generation checks the loaded backend against the pinned tokenizer rather than
applying automatic tokenizer rewrites. Generated manifests record versions and
payload SHA-256 values. Python packages and PowerShell are not inference dependencies.

The C API lives in `src/tools/gemma/gemma_tokenizer.h`. Keep validated artifact
bytes alive and immutable while the tokenizer is in use. Each concurrent request
needs its own caller-owned `GemmaTokenizerWork` (5,701,636 bytes) and output buffers;
the table view itself is read-only and shareable. Calls allocate no memory and
report failure with zero output count/length; discard partial buffer contents on
failure. Input/output buffers must not overlap. Input UTF-8 is validated, input
bytes are capped at 196608, and the complete prompt plus output budget must fit
2048 tokens. Streaming generation is deferred to the translation runtime stage.

### Numerical references

Stage 5 uses the same build entry point. Once its artifacts exist, run the
tokenizer and numerical C tests, including the no-CRT PE audit, without Python:

```powershell
.\experimental\snapdragon\tools\build-gemma.ps1 -TestNumerics
```

Full offline regeneration reads the pinned source checkpoint and actual Stage 3
W8/W4 artifacts, records primitive/local/global layer traces and logits, and
generates the multilingual corpus. BF16 greedy decoding is repeated for exact
token agreement. This CPU reference is intentionally not an inference product
or a performance benchmark; regeneration can take substantially longer than
the C tests. It uses the existing NumPy/tokenizer reference environment:

```powershell
.\experimental\snapdragon\tools\build-gemma.ps1 -ExportNumerics -TestNumerics
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe experimental/snapdragon/tools/export-translategemma.py --numerical-reference --verify-only
```

Output lives under `models/translategemma-4b-stage5-v2/`. Its manifest describes the
effective arithmetic, array dimensions, payload hashes, source identity, prompt
IDs, generated IDs, decoded translation hashes, and repeated-BF16 status. The
reference uses FP32 BLAS accumulation followed by explicit BF16/FP16 rounding;
it is not asserted to be bit-identical to PyTorch or HTP kernels. Version 2 uses
the checkpoint's global linear RoPE factor 8 (local factor 1), independently
confirmed with Transformers 5.17.0. The old factor-1 schema remains preserved in
`models/translategemma-4b-stage5/` but is rejected by current verification.

W8/W4 residuals are stored as `hidden / 32`. Embedding and post-norm gains are
divided before FP16 rounding; pre/final RMSNorm epsilon is `1e-6 / 1024`.
BF16 remains unscaled. The HTP model-width scaled-add/RMSNorm probe passes,
including small values sensitive to epsilon. Plain FP32 add IO did not preserve
out-of-FP16-range sums on this runtime, so it is not a substitute for scaling.
The Stage 5 plan defines the exact arithmetic and Stage 6 validation obligations.

Quantized FP16 overflow is preserved as a diagnostic artifact, not clipped into
a plausible translation. A complete reference manifest can therefore have
`quantized_generation_ready=false`; inspect each translation's `status` and
`error`. The historical unscaled failures (layer 5, W8 73520 and W4 71568) were
independently reproduced with stock PyTorch decoder layers. With divisor 32,
all full-depth acceptance prompts reach finite logits. Finite generation alone
does not establish translation quality or full-block HTP accuracy.
Responses that exhaust the 64-token budget are preserved with `status=token_limit`
and their actual generated tokens; they are not successful translations and
keep `quantized_generation_ready=false`. BF16 still requires exact terminated
replay. W4's observed Japanese early-token divergence also persists with a
diagnostic FP32 residual stream, independent of scaled FP16 storage.

The published version-2 corpus contains 184 verified artifacts. W8 matches all
three BF16 token sequences exactly, with no overflow. W4 stays finite but has
degraded Czech output, a repetitive Japanese `token_limit` response, and changed
German-to-English text. Its quality/generation gate remains blocked. Proceed
with W8 first when evaluating full-model QNN generation; finite W4 arithmetic
is not translation acceptance. Stage 6 local/global block checks now pass both
W8 and W4 as described below.

### Translation Quality Tests

The separate [quality corpus](../tools/translategemma-quality.json) contains 24
diagnostic cases and 24 reserved held-out cases, without changing the published
three-sentence fixtures. References have AI semantic approval by GitHub Copilot
at the user's request; this is not independent human review. The
[quality runner](../tools/translategemma-quality.py) prepares exact prompts,
compares BF16/W8/W4, records termination/repetition flags, exports variant-blinded
review packets, and scores pinned chrF++. Held-out use requires attributed semantic review
and a matching frozen case hash. See the [evaluation workflow](plan-translategemma.md#quality-evaluation-workflow).

The hardware probe also has 12 tiny basis/sign/cancellation cases. All 48 per-axis
outputs are exact; direct mapped grouped encoding, explicit dequantization and
mapped expansion fail 39/48, with results matching unscaled integer weights.
An opt-in composition of per-axis group MatMuls plus FP16 Adds passes all 48
outputs and a 2560-wide projection. This is a working projection control, not
full-model grouped-W4 acceptance or a performance result.

The quality runner also supports offline group-32 candidates and targeted W8
layer/head substitutions, atomic checkpoints, and provenance-checked resumption.
`freeze` requires a reviewed 24-case diagnostic pass; held-out evaluation binds
the chosen candidate and decoding budget and reports pilot semantic quality
separately from deployment acceptance. The baseline and group-32 diagnostic
campaign was stopped at the user's request on 2026-09-16 because completion
within another 30 minutes was not realistic. All 24 BF16 and eight W8 outputs
are preserved; W4 and group-32 campaign runs did not start. No candidate has
been frozen or evaluated on the held-out set. Do not restart automatically.

### Full-Model Memory Measurement

The offline Windows sampler `tools/measure-translategemma-memory.py` measures the
existing Stage 7 full-depth W4 restore/prompt gate without rebuilding contexts or
changing production binaries. It records process working set/private bytes,
peak working set/commit, system available RAM/commit and the native test log.
It refuses to launch below 8 GiB available RAM. Run after the offline quality
campaign has finished, using a new report path for each bucket:

```powershell
./experimental/snapdragon/build/calibration-venv/Scripts/python.exe -B experimental/snapdragon/tools/measure-translategemma-memory.py --measure --bucket 512 --output experimental/snapdragon/models/translategemma-memory-512.json
```

Repeat serially for buckets 1024 and 2048. Omit `--measure` for preflight only.
The September 16 preflight found 1,564,086,272 available physical bytes and
correctly did not launch QNN. The 512 context occupies 1,966,356,568 bytes on disk;
this is not its resident memory requirement. Actual full-model memory results
remain pending. Native Windows counter tests pass, but the complete sampler run
has not yet been exercised against QNN.

Process counters do not account separately for DSP/driver memory, and system
deltas include other applications. Sampling can miss brief peaks; process peak
counters supplement it. This measures restore and 253/256-token prompt execution,
not graph construction or autoregressive decoding. In particular, the serialized
context buffer exists during deserialization, so loading peaks must be considered.

12B feasibility remains an estimate: approximately 6 GB packed W4 weights plus
0.75 GB group-32 FP16 scales, before KV/activations/runtime/loading copies. Do not
extrapolate resident memory solely from the 4B context file size. No 12B weights
have been downloaded or tested; retrieving the official configuration without
authentication returned HTTP 401. Establish the 4B measured envelope first.

### QNN Block Validation

Stage 6 builds a separate no-CRT ARM64 runner and tests local layer 0 and global
layer 5 in W8 then W4, using the actual Stage 3 weights and v2 scaled residual
contract. Normal builds/tests use Clang/LLVM, PowerShell, and QNN, without Python:

```powershell
.\experimental\snapdragon\tools\build-gemma.ps1 -TestBlocks -TestNumerics
.\experimental\snapdragon\tools\build-gemma.ps1 -TestBlocks -BlockBits 8 -BlockLayers 0
```

The dedicated binary, binding files, and per-block/cleanup logs live in
`build/gemma-block/`. Signed QNN `.dll`, DSP `.so`, and `.cat` files are copied
from the selected build directory; keep catalogs with the DSP libraries.
Production Whisper binaries and contexts are not overwritten. The runner
requires the tested QNN core 2.39 API and imports only Kernel32 statically.

Generate the small cached-block reference set once with the existing offline
NumPy environment; this does not regenerate the full translation corpus:

```powershell
$env:OPENBLAS_NUM_THREADS = '4'
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe -B experimental/snapdragon/tools/export-translategemma.py --block-reference --replace
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe -B experimental/snapdragon/tools/export-translategemma.py --block-reference --verify-only
```

The 209 artifacts in `models/translategemma-4b-stage6/` cover two three-token
executions per block, sparse positions crossing the local-window boundary,
runtime RoPE positions, and prior KV consumption. All four hardware cases pass
20 intermediate comparisons per step, masked-zero checks, shared-memory guards,
cached-row influence/restoration, and deterministic warm replay. Five injected
failure stages pass cleanup checks. The retained diagnostic outputs and internal
KV expansion make these correctness graphs, not the final prompt/decode design.
The [Stage 6 report](plan-translategemma.md#stage-6-qnn-transformer-block) records
tolerances, timing results, fixture limits, and remaining full-model obligations.

### Prompt Processor Bring-up

Stage 7 is complete for all three context buckets (512, 1024, 2048). The 128-token, 34-layer W4
graph shares one runtime position input across layers and uses FP32 index
inputs with an internal Cast to avoid HTP integer-input lookup errors.
Fresh-process restore passes all-layer KV/logit tolerances, padding, guards,
deterministic replay, and the throughput gate at 488/449/353 input tokens/s,
respectively, using the 253/256-token fixtures. Exact IO-schema validation,
75 envelope-corruption cases, and nine truncated-file cases pass. Existing
Stage 6 gates also pass. Stage 5 W4 translation quality remains independently blocked.
See the [Stage 7 status](plan-translategemma.md#stage-7-prompt-processor) for the
isolation results and version 4 context contract.

```powershell
$env:OPENBLAS_NUM_THREADS = '4'
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe -B experimental/snapdragon/tools/export-translategemma.py --prompt-reference --replace
foreach ($bucket in 512, 1024, 2048) {
	.\experimental\snapdragon\tools\build-gemma.ps1 -TestPrompt -PromptBucket $bucket
	.\experimental\snapdragon\tools\build-gemma.ps1 -TestPrompt -RestorePrompt -PromptBucket $bucket
}
```

Restore-only requires the context written by the build command. Both commands
run the position and envelope-corruption regressions, then validate the full
prompt and reject truncated context files. The context, bindings, and logs
remain separate under `build/gemma-block/`.

### Independent Numerical Checks

Optional independent development checks reuse the existing test file:

```powershell
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/test-translategemma-stage3.py --torch-oracle
$env:OPENBLAS_NUM_THREADS = '4'
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe -B experimental/snapdragon/tools/test-translategemma-stage3.py --residual-audit
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe -B experimental/snapdragon/tools/test-translategemma-stage3.py --residual-rounding-audit
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe -B experimental/snapdragon/tools/test-translategemma-stage3.py --residual-bound-audit
```

The first command requires the isolated x64 Python 3.14 environment with torch
2.14.0, Transformers 5.17.0, and NumPy 2.4.3. It runs under Windows emulation,
not in the production path; ordinary build/test/export does not require it.

For an isolated HTP capability build, keep the signed runtime together:

```powershell
.\experimental\snapdragon\tools\build.ps1 -BuildDir experimental/snapdragon/build/gemma-precision-probe
Get-ChildItem experimental/snapdragon/build -File | Where-Object { $_.Extension -in @('.dll', '.so', '.cat') } | Copy-Item -Destination experimental/snapdragon/build/gemma-precision-probe
.\experimental\snapdragon\tools\test-gemma-stage1.ps1 -BuildDir experimental/snapdragon/build/gemma-precision-probe -SkipBuild
```

The `.cat` files are required alongside the DSP `.so` files. Omitting them can
force QNN onto the user-driver path, where the existing RMSNorm capability test
fails. This separate build leaves the retained production Whisper binary intact.

For a fast scalar-only development check, deliberately publish to scratch space:

```powershell
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe experimental/snapdragon/tools/export-translategemma.py --numerical-reference --primitives-only --output tests/tmp/gemma-numeric-primitives --replace
.\experimental\snapdragon\tools\build-gemma.ps1 -TestNumerics -NumericDir tests/tmp/gemma-numeric-primitives
```

Such a set has `complete=false` and cannot substitute for the full Stage 5 gate.
The scalar fixtures cover all FP16 bit patterns, W4 nibble order and signs, W8
dequantization, split-half RoPE, mask boundaries, KV ring offsets, and first-index
argmax ties and invalid logits. The reusable numerical helpers have no allocation,
OS, QNN, standard-library, or scripting dependency.

## Retained local runtime (2026-09-15)

The local `build/` directory now contains only the benchmarked fused-self Medium
`npu_probe.exe`, its `whisper-medium-m1c-self-fused-v1-encoder-fp16-l24.qnnctx`,
and the matching QNN runtime, DSP files, licenses, and version manifest.
Run from the repository root:

```powershell
.\experimental\snapdragon\build\npu_probe.exe .\experimental\snapdragon\data\long-form-35s.wav
```

Medium and `fused,self,logits` are implicit defaults. The executable still needs
the existing exported artifacts under `models/`; `build/` alone is not a portable
distribution. All source models and saved reports were retained. Other models
and offload modes require regenerating their removed contexts.

Cleanup reduced `build/` from 11,207,142,797 to 1,685,382,549 bytes. Four console
cancellation cases passed; transcription before and after removal matched the
35-second reference, with 6,384 fused self-attention submissions and no CPU self
fallback. Inventory and verification logs are in
`data/build-cleanup-20260915/`, including the preserved `diagnostics-symbols/`
and `bitcast-symbols/` executable/PDB pairs. Historical paths in benchmark and
trace reports describe the original runs, not the current deployment.

Build defaults have not changed: use `tools/build.ps1 -SelfFusionCandidate` to
rebuild this variant in its isolated directory, then copy its `npu_probe.exe`
into `build/` while preserving the matching fused context and QNN runtime.
Do not use `-Clean` on the retained runtime directory. Repeating historical
multi-variant benchmarks requires rebuilding their binaries and contexts;
`benchmark-whisper-self-fusion.ps1 -ReportOnly` still uses the saved results.

## Inventory

Run the PowerShell inventory from the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\inventory.ps1
```

The inventory reports the host, processor, memory, display and compute adapters, relevant system runtimes, and available compilers.

## Native probe

Build and run the ARM64 probe with LLVM-MinGW Clang:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\build.ps1 -Clean
.\experimental\snapdragon\build\probe.exe
```

The probe reports Windows ARM64 processor features, enumerates DXCore adapters by hardware type and D3D12 capability, and attempts D3D12 plus DirectML device creation for every adapter advertising `D3D12_GENERIC_ML`.

## Direct QNN NPU probe

Build the freestanding probes, stage the pinned official QNN HTP runtime, and exercise its provider, lifecycle, and graph ABI:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\build.ps1 -Clean
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\fetch-qnn-runtime.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\fetch-whisper-tiny.ps1
python .\experimental\snapdragon\tools\prepare-whisper-tiny-mlp.py
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\fetch-whisper-calibration.ps1
python .\experimental\snapdragon\tools\calibrate-whisper-tiny-mlp.py
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe .\experimental\snapdragon\tools\export-whisper-decoder.py --model tiny
.\experimental\snapdragon\build\npu_probe_builder.exe --model=tiny
.\experimental\snapdragon\build\npu_probe.exe .\experimental\snapdragon\data\long-form-35s.wav
```

For Whisper Base, fetch and export its pinned checkpoint, build its independent QNN context, and select it at runtime:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\fetch-whisper-base.ps1
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe .\experimental\snapdragon\tools\export-whisper-decoder.py --model base
.\experimental\snapdragon\build\npu_probe_builder.exe --model=base
.\experimental\snapdragon\build\npu_probe.exe --model=base .\experimental\snapdragon\data\long-form-35s.wav
```

Whisper Small follows the same model-specific path:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\fetch-whisper-small.ps1
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe .\experimental\snapdragon\tools\export-whisper-decoder.py --model small
.\experimental\snapdragon\build\npu_probe_builder.exe --model=small
.\experimental\snapdragon\build\npu_probe.exe --model=small .\experimental\snapdragon\data\long-form-35s.wav
```

Medium is the default model, with `--decoder-offload=fused,self,logits` as the default offload mode. Explicit `--model` and `--decoder-offload` flags override their respective defaults independently. Cache files and QNN graph names are model-specific, so other models never restore an incompatible context.

Whisper Medium uses the same FP16 path with width 1024, 16 attention heads,
and 24 encoder and decoder layers. Prepare its pinned multilingual checkpoint
and model-specific context with:

```powershell
.\experimental\snapdragon\tools\build.ps1
.\experimental\snapdragon\tools\fetch-whisper-medium.ps1
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe .\experimental\snapdragon\tools\export-whisper-decoder.py --model medium
.\experimental\snapdragon\build\npu_probe_builder.exe --model=medium
.\experimental\snapdragon\build\npu_probe.exe --model=medium .\experimental\snapdragon\data\long-form-35s.wav
```

The 24-layer context metadata layout uses `whisper-<model>-encoder-fp16-l24.qnnctx`
for Tiny/Base/Small. Medium builds only the selected decoder offloads and includes
their hexadecimal mask in the filename: `whisper-medium-m03-encoder-fp16-l24.qnnctx`
for `cross,mlp`, and `whisper-medium-m18-encoder-fp16-l24.qnnctx` for
`fused,logits`. Use the same offload option when building and transcribing.
Rebuild each selected model's context when upgrading from the 16-layer executable;
the old unsuffixed caches are neither loaded nor overwritten. Model weight bundles
retain the version-2 artifact format. With no model/offload flags, the builder and
runtime select Medium's `-m1c-` context (`fused,self,logits`).

Medium's source checkpoint is 3,055,544,304 bytes; its FP16 decoder weights alone
occupy about 871 MiB. QNN contexts, graph compiler allocations, caches, and retained
CPU fallback weights add to this. Measure both context-build and transcription
memory on the target machine. An initial Medium context containing every decoder
graph variant reached 2,073,598,736 bytes and failed DSP weight-buffer mapping and
context restore. Mode-specific contexts avoid the unused graph variants; the
smaller models retain their existing all-variant contexts.

On the current 16 GB Surface Laptop and QAIRT 2.50 runtime, a single Medium
`fused,self,logits` context fails restore after pruning. A fresh runtime process
reproduces the failure, so builder leftovers are not its sole cause. QNN estimates
2,857,368,320 bytes of context memory; this is an estimate, not a measured hardware
limit. Debug logs show successful host-side weight mappings followed by DSP
deserialization error 5005 on each attempted process domain.

The loader now selectively restores Medium modes with NPU self-attention into
separate decoder and frontend/encoder contexts, using the same FP16 cache binary.
The full decoder subset restores with an estimated 1,956.58 MiB requirement;
the frontend/encoder subset needs an estimated 768.42 MiB. On this machine QNN
places the decoder on PD 0, first attempts the encoder on PD 0, then successfully
retries on PD 2. Both contexts remain resident across audio windows and are freed
at shutdown. No quantization or per-window context reload is required. The QNN
log adapter defers only known placement-retry errors until restore returns. A
successful retry produces one recovery warning in normal mode and no diagnostic
in `--quiet` mode. A failed restore replays the buffered errors; unrelated errors
remain visible immediately. The retry itself still occurs: reversing context
order and enabling I/O estimation did not eliminate it on this SDK.

The main `build/npu_probe.exe` and the isolated `build/memory-candidate/` build
default to Medium with `fused,self,logits`. The I/O-reuse
experiment lowered the single-context estimate by about 97 MiB but did not fix
deserialization. V81-only extended-uDMA mapping is not applicable to the V73
graphs used here. Closing applications or increasing system RAM has not been
demonstrated to resolve this process-domain constraint.

The split-context candidate completed the five-minute fixture with 12 windows,
1,352 tokens, 106,704 NPU self-attention submissions, and exit zero. Its transcript
SHA-256 matches both earlier five-minute modes. One sample measured 167.286 s
elapsed, 92.063 CPU-seconds, and 3.44 GiB peak resident memory, versus the earlier
`fused,logits` sample's 167.359 s and 106.375 CPU-seconds. This establishes working
full FP16 offload, not a repeatably measured latency improvement. Host-call duty
was 76.78%; it is not hardware occupancy. Results are in
`data/medium-memory-investigation/full-5min/`.

Ctrl+C and Ctrl+Break request cooperative cancellation. The console handler only
sets an atomic flag; the inference thread checks it between decoder steps and
frontend/encoder stages, then releases its workers, graphs, contexts, and backend
normally. Completed transcript windows remain on stdout; an unfinished window
is not committed. Shutdown writes `npu_probe: interrupted; resources released.`
to stderr and exits with status 130. Repeated interrupts continue to request the
same orderly shutdown. An active QNN call must return before cleanup can begin;
closing the console, ending the process in Task Manager, or a hung driver cannot
be guaranteed graceful cleanup. The optional development-time hardware check is
`tools/test-whisper-cancellation.py`, which sends actual Ctrl+C/Ctrl+Break events
inside isolated Windows consoles without signalling the user's terminal.

The updated candidate passes the strict 15-profile hardware regression including
Medium `fused,self,logits`, the no-CRT/mock suite, and four actual console-event
cases (startup Ctrl+C, decoding Ctrl+C/Ctrl+Break, and quiet long-form Ctrl+C).
The final quiet two-window run preserves the transcript byte-for-byte and has
empty stderr. Hardware results are under `data/medium-clean-lifecycle-validation/`
and `data/medium-clean-lifecycle-final/`.

The `cross,mlp` context restores and transcribes successfully; its first
35.008-second two-window smoke run took 36.016 seconds and 33.641 CPU-seconds,
with 132 generated tokens and about 2.76 GiB peak resident memory. Repetition
retries contributed substantially to this result. It is not a steady-state
speed or transcription-quality benchmark.

The lower-memory `fused,logits` mode also restores and transcribes successfully.
It keeps decoder self-attention on the CPU while offloading fused cross-attention
and MLP plus final vocabulary projection:

```powershell
.\experimental\snapdragon\build\npu_probe_builder.exe --model=medium --decoder-offload=fused,logits
.\experimental\snapdragon\build\npu_probe.exe --model=medium --decoder-offload=fused,logits .\experimental\snapdragon\data\long-form-35s.wav
```

Its first matched-clip smoke run took 29.123 seconds and 25.484 CPU-seconds,
with about 2.91 GiB peak resident memory. It generated 137 tokens rather than
132 in `cross,mlp`; the transcript changes with offload precision. These single
runs are not a repeated performance comparison. Use `--decoder-offload=fused,logits`
explicitly to select this lower-memory mode instead of full decoder offload.

The final hardware regression repeated both Medium modes at 34.888 seconds /
32.328 CPU-seconds (`cross,mlp`) and 33.581 seconds / 28.750 CPU-seconds
(`fused,logits`), preserving each mode's first-run transcript. All six
Tiny/Base/Small model/mode comparisons matched the original executable's
transcript hashes and token counts. That earlier deployment of the main
`build/npu_probe.exe` and `build/npu_probe_builder.exe` added support for Tiny,
Base, Small, and Medium. Its runtime matched the then-validated
`build/medium-candidate/npu_probe.exe`, with historical SHA-256
`627ecfd72c850fae237c968aed6103f98c28211c586c79b582637aadc52cca6d`.

For isolated bring-up, build with
`tools/build.ps1 -BuildDir experimental/snapdragon/build/medium-candidate`
and stage the same QNN DLLs,
DSP `.so` files, and `.cat` files beside the candidate. The original executable
and unsuffixed contexts can then remain in place for comparison. After building
the three smaller `-l24` contexts and Medium's `cross,mlp` and `fused,logits`
contexts, run
`experimental/snapdragon/tools/test-whisper-medium.ps1` from the repository root.
It defaults to the isolated candidate and existing 35-second WAV, checks
Tiny/Base/Small transcripts against `-ReferenceProbePath` (the main build by
default) in both decoder
modes, and records Medium CPU time, elapsed time, memory, and NPU submissions
under `data/medium-validation/`. These are hardware smoke measurements, not a
transcription-quality benchmark. The optional `-MediumModes` array selects
additional mode-specific contexts to test.

The decoder default is `--decoder-offload=fused,self,logits`, selected for full NPU decoder offload and reduced CPU work, not a guaranteed latency advantage. Use `cpu`, `all`, or a comma-separated subset of `cross`, `mlp`, `self`, `logits`, and `fused` for controlled measurements. `fused` replaces separate cross-attention and MLP graphs and cannot be combined with `cross` or `mlp`. To restore the former defaults, pass `--model=small --decoder-offload=cross,mlp`.

From `experimental/snapdragon`, the normal invocation is now:

```powershell
.\build\npu_probe.exe --quiet .\data\bundestag-hearing-16k-mono-f32.wav
```

A WAV argument is still required for transcription; invoking the runtime without
an input prints usage. Build the default context with `build/npu_probe_builder.exe`
if it has not been prepared yet.

Pass a 16 kHz mono float32 WAV as the first argument to transcribe the complete track through the cached frontend and encoder. For conversion, retries, resumable records, and richer reports, use the development-time driver:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\transcribe-long-form.ps1
```

The driver creates `data/bundestag-hearing-16k-mono-f32.wav`, extracts overlapping 30-second inference windows, invokes the freestanding probe with raw UTF-8 pipes, repairs capped or sustained-repeat windows on `-Resume -RetryFlagged`, and emits a stitched transcript, timestamped raw segments, JSONL records, CSV timings, and a timing summary under `data/bundestag-hearing-transcription/`. FFmpeg and PowerShell are orchestration tools only; the deployed inference executable remains freestanding C. A single window can still be prepared manually:

```powershell
ffmpeg -ss 300 -i .\experimental\snapdragon\data\7654653_mp3_128kb_stereo_de_128.mp3 -t 30 -ac 1 -ar 16000 -c:a pcm_f32le .\experimental\snapdragon\data\bundestag-hearing-300s-30s-f32.wav
.\experimental\snapdragon\build\npu_probe.exe .\experimental\snapdragon\data\bundestag-hearing-300s-30s-f32.wav
```

A plain mono 16 kHz `pcm_f32le` WAV argument is transcribed to the end of the track. The native probe seeks through overlapping 30-second windows at a 25-second stride, keeps QNN and decoder state loaded, and prints each new deduplicated `Transcript update (stitched)` as soon as its window completes. It also prints `Full transcript (German, greedy)` after the final window. Use a quoted `--single-window=<path>` argument only when deliberately limiting inference to the first 30 seconds:

```powershell
.\experimental\snapdragon\build\npu_probe.exe '--single-window=.\experimental\snapdragon\data\bundestag-hearing-16k-mono-f32.wav'
```

Use `--quiet` when stdout should contain only the progressively stitched transcript, with no probe headings, timing data, segment markers, or repeated final transcript:

```powershell
.\experimental\snapdragon\build\npu_probe.exe --quiet .\experimental\snapdragon\data\bundestag-hearing-16k-mono-f32.wav
```

The equivalent single-argument form is `--quiet=<path>`. Quiet mode still reports failures on stderr and returns a nonzero exit code; run without `--quiet` for detailed diagnostics. The existing output remains the default for diagnostics and manifest-driven orchestration.

Model artifacts are located when running from the repository root, `experimental/snapdragon`, or its `build` directory. WAV paths remain relative to your current directory. For example, from `experimental/snapdragon`:

```powershell
.\build\npu_probe.exe --quiet --model=small --decoder-offload=fused,self,logits .\data\bundestag-hearing-16k-mono-f32.wav
```

The long-form driver writes one absolute WAV path per line to `window-manifest.txt` and invokes the native batch interface once:

```powershell
.\experimental\snapdragon\build\npu_probe.exe '@C:\path\to\window-manifest.txt'
```

The probe restores the selected model's QNN context, loads its decoder bundle, and starts the `RtTaskPool` once, then processes every manifest entry in order. Structured `WHISPER BATCH SEGMENT` markers let the driver retain individual logs and resume records. Use `-PerWindowProcesses` only when comparing or debugging process isolation.

The external-audio path bypasses the diagnostic graph suite, reports FNV-1a fingerprints for intermediate outputs, and transcribes with a plain-C incremental decoder. German transcription uses the fixed prompt `<|startoftranscript|><|de|><|transcribe|><|notimestamps|>`, descriptor-sized self-attention KV caches, and byte-level token decoding. Tiny retains greedy selection. Base retains bounded deterministic temperature retries when repeated token bigrams and trigrams cross its established threshold. Small retries only repeated trigrams; its temperature retry forbids repeated trigrams online and therefore needs at most one retry. A cached FP16 QNN graph computes encoder normalization and all descriptor-selected cross-attention K/V projections once per audio window into registered FastRPC shared memory. Per-layer HTP graphs consume those head-major K/V buffers directly for cross-attention without copying the complete cache per token; CPU cross-attention remains the fallback. Mutable self-attention K/V shares the registered allocation, and CPU updates only the current FP16 row between an HTP projection graph and an HTP masked-attention/output graph. A final HTP graph applies LayerNorm and the complete vocabulary projection; CPU retains exact suppression and candidate selection over its FP16 logits. CPU implementations remain available for every decoder offload. Decoder, token, cross-K/V, self-attention, final-projection, frontend, and encoder bundles are generated atomically with:

```powershell
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe .\experimental\snapdragon\tools\export-whisper-decoder.py --model tiny
```

The exporter validates checkpoint `config.json` against the pinned model catalog and atomically writes all eight model bundles. Every production model artifact, including the QNN context cache, starts with the shared 96-byte version-2 header containing model identity, complete dimensions, payload and element types, 64-bit counts and sizes, and an FNV-1a payload hash. Runtime readers reject unsupported versions, wrong models or dimensions, truncation, overflow-sized fields, and hash mismatches before inference.

Python and NumPy remain development-only exporters. Runtime token generation and detokenization use freestanding C without a standard library. Decoder weights, token storage, layer bindings, attention caches, scratch vectors, task-pool state, and profiling belong to an explicit runtime-sized `WhisperDecoder` context; checked 16-byte-aligned arenas replace Tiny-sized global buffers. The native command handles complete compatible WAV tracks and prints a stitched transcript. The PowerShell driver remains useful when compressed-input conversion, resumable per-window records, timestamped output, retry repair, and aggregate profiling are required. Configurable language/task prompts remain separate work.

`npu_probe.exe` is a no-CRT ARM64 PE that imports only `KERNEL32.dll`. It dynamically loads the unavoidable proprietary `QnnHtp.dll` backend, obtains its QNN 2.39 function table, and creates logging, backend, device, profile, and context handles; it does not load ONNX Runtime. Freestanding C parses a fixed 16 kHz WAV and computes Whisper log-mel features. Two cached FP16 QNN graphs run the frontend convolutions, GELUs, and position addition before passing their result directly to a model-generated encoder graph: 88 nodes for Tiny, 132 for Base, or 264 for Small. Runtime activation buffers and temporary builder weights are descriptor-sized; builder weights are released after context restoration. Handles are released in reverse order on success and failure.

The diagnostic path reports peak/current working set and private committed bytes through `GetCurrentProcess` and `K32GetProcessMemoryInfo`, both exported by Kernel32 on the supported Windows target. The original Small CPU-decoder reference peaked at 715,628,544 resident bytes and 494,174,208 private committed bytes. The current all-ablation context, including separate and fused decoder graphs, reached about 1.63 GB resident while private committed memory remained about 498 MB. Production-context pruning is required before treating that measurement as a deployment budget. See [snapdragon-x-qnn.md](snapdragon-x-qnn.md) for reusable Windows on Snapdragon and QNN integration findings.

The stable decoder uses separate cached cross-attention and MLP graphs. The experimental `fused` mode performs cross-attention, both residuals, final LayerNorm, and MLP in one graph per layer, halving those submissions. Stateful self-attention uses two HTP submissions per layer and final LayerNorm plus vocabulary projection uses one submission per token. All paths retain CPU fallbacks and exact CPU suppression/sampling. Fusion improves focused Tiny and Small latency, but the first five-minute Small run remained slower overall; `fused,self,logits` raised host-call duty to 61.2% and reduced CPU time by 14.0% versus the same-context stable mode while missing its wall-time gate by 1.5%. Detailed timings and the repeatable `benchmark-whisper-offloads.ps1` runner are documented in [benchmark.md](benchmark.md).

The QNN staging script verifies the pinned official `data/v2.50.0.260828.zip` archive, confirms its QAIRT and QNN API metadata, and extracts the Windows ARM64 HTP/System files, V73/V81 skels, notices, and hashes into the ignored build directory. The latest validated SDK is the single working baseline: an upgrade replaces the staged runtime and contexts rather than creating parallel active versions. The model script pins multilingual `openai/whisper-tiny` revision `169d4a4341b33bc18d8881c4b69c2e104e1cc0af` and verifies the checkpoint's size and SHA-256. Calibration uses one pinned validation clip from each of 16 FLEURS languages. Model and corpus licenses, source revisions, file hashes, quantization errors, calibrated encodings, and deployment artifact hashes are recorded under the ignored `build/` tree. Python and NumPy are development-time preparation tools only; the deployed graph loader remains freestanding C. Review `build/qnn-licenses/LICENSE.pdf`, the accompanying notices, the model card, and the FLEURS CC BY 4.0 attribution before redistribution.

The narrow ABI in `../src/shared/qnn_abi.h` was checked against exact QAIRT `2.50.0.260828` QNN core 2.39 headers from the pinned official SDK archive. Compiler-derived ARM64 sizes and offsets are enforced with static assertions; no offsets are inferred from binaries and no proprietary SDK headers are copied into the repository. The validated provider on this machine is `HTP_QTI_AISW`, backend ID 6, with QNN core API 2.39.0 and HTP backend API 5.50.0. See [npu-plan.md](npu-plan.md) for the incremental path to a Whisper-like model.

Run the deterministic failure-path suite with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\test-npu-probe.ps1
```

The suite first runs no-CRT ARM64 checks for 20 version-2 artifact contract cases, including Medium dimensions and model isolation, a complete 24-layer context metadata round trip, and all four decoder allocation-failure cleanup points. It then builds freestanding ARM64 mock provider DLLs and runs 12 cases covering loader/provider errors, QNN 2.39 compatibility, required functions, reverse cleanup after lifecycle and graph failures, output corruption detection, and successful Add plus Whisper Tiny MatMul execution.

Generate a development-time FP16 encoder-layer bundle with:

```powershell
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe .\experimental\snapdragon\tools\calibrate-whisper-tiny-mlp.py --layer 0 --fp16-only
```

Python and NumPy are used only to verify the pinned inputs and emit raw FP16 artifacts. The deployed `npu_probe.exe` path is freestanding C with no CRT or standard-library dependency; it reads those blobs through Kernel32 and submits the graph directly through QNN.

## Current machine result

For CPU-first Small transcription, explicitly select the measured high-offload path:

```powershell
.\experimental\snapdragon\build\npu_probe.exe --model=small --decoder-offload=fused,self,logits <compatible.wav>
```

Rebuild the executable with `tools/build.ps1` before using the decoder work-reuse changes; existing QNN contexts remain compatible. Three interleaved five-minute before/after pairs in this mode reduced median CPU-seconds from 70.91 to 27.17 and elapsed time from 75.83 to 57.24 seconds, with identical transcripts. Median NPU host-call duty rose from 59.35% to 78.69%; it is not a hardware occupancy measurement. Optional exact sampling caching increases private commitment by roughly 94 MB. See [benchmark.md](benchmark.md) for binary identities, variability, and validation. Changing the offload mode itself can change model output, even though these work-reuse changes preserve each tested mode's transcript.

On the Surface Laptop 7 used for bring-up:

- Adreno X1-85 creates both an `ID3D12Device1` at `D3D_FEATURE_LEVEL_1_0_GENERIC` and an `IDMLDevice`.
- Microsoft Basic Render Driver also creates both devices, providing an independent ABI control.
- Hexagon NPU is enumerated as a hardware NPU and advertises `D3D12_GENERIC_ML`, but `D3D12CreateDevice` returns `0x887a0004` (`DXGI_ERROR_UNSUPPORTED`). DirectML creation is therefore not attempted for it.
- The active NPU package is `qcnspmcdm8380.inf`, version `30.0.220.3000`, and includes Qualcomm's DXCore user-mode driver.
- Direct QNN lifecycle creation succeeds through `QnnHtp.dll`: all five create calls and all five reverse-order free calls return zero.
- Direct QNN graph execution succeeds on HTP: graph creation, tensor registration, node addition, finalization, and execution return zero, and every quantized Add output byte matches the CPU reference.
- One bring-up run measured 147.3 ms graph creation, 17.3 ms finalization, 1.87 ms first execution, and 156.4 us warmed median host-call latency across 100 samples (p95 259.8 us). Treat this tiny graph as a dispatch baseline, not a throughput result.
- Whisper Tiny `[1,384] x [384,384]` MatMul passes exact scalar validation with 173.0 us warmed median latency and 0.852 GMAC/s median throughput.
- Whisper Tiny `[1500,384] x [384,384]` MatMul passes all 576,000 output-byte checks with 314.7 us warmed median latency and 702.840 GMAC/s median throughput.
- Whisper Tiny MLP expansion `[1500,384] x [384,1536]` passes all 2,304,000 output-byte checks with 692.9 us warmed median latency and 1,276.859 GMAC/s median throughput.
- Whisper Tiny MLP contraction `[1500,1536] x [1536,384]` passes all 576,000 output-byte checks with 738.3 us warmed median latency and 1,198.342 GMAC/s median throughput.
- The model-derived layer-0 `FullyConnected -> Gelu -> FullyConnected` graph passes all 576,000 output-byte checks against the offline quantized fixture with maximum byte delta 1. It measured 58.084 ms finalization, 2.889 ms first execution, 452.7 us warmed median latency, and 3,908.707 GMAC/s median throughput for both projections together.
- Model-derived Q/K/V projections each stay within one UINT8 code of their offline fixtures. The full rank-3 attention core executes `MatMul -> Softmax -> MatMul`; its reference run had maximum byte delta 14 with 139 of 576,000 values over two codes. The reshape/transpose layout path is byte-exact.
- The complete 21-node static encoder block finalizes and executes on HTP. Its output has mean absolute delta 1.366 codes and relative L2 4.94% against the offline quantized fixture; the maximum delta is 19, with 283 of 576,000 values over eight codes and three over sixteen. One run measured 216.108 ms finalization, 1.907 ms first execution, and 1.729 ms warmed median latency across 100 samples (p95 7.108 ms, mean 2.373 ms), or 2,535.266 GMAC/s for the projection and attention MAC count.
- All four encoder layers now have independent 16-language calibration bundles and pass the complete-block HTP probe. Warm median latencies were 1.701, 1.670, 1.734, and 1.756 ms for layers 0 through 3.
- Four finalized layer graphs execute as a chained encoder in 7.792 ms median, 6.686 ms minimum, and 14.033 ms p95, sustaining 2,249.535 GMAC/s. The chained HTP result remains close to a cascaded UINT8 simulator with mean absolute delta 1.211 codes, but the cascaded UINT8 model itself has 0.796 relative L2 error against the float encoder. Monolithic composition and context caching are therefore deferred until the activation-precision decision is resolved.
- Wider-precision probes show exact UINT16 residual addition, but quantized 16-bit FullyConnected is not viable on this runtime: direct W8A16 and QNN's UINT16-weight conversion to symmetric W16A16 both discard the lower eight output bits. FP16 FullyConnected finalizes, executes, and matches the exact half-precision reference.
- All four 22-node encoder blocks run independently, as a chained FP16 encoder, and as one 88-node graph. Cumulative relative L2 error against the float encoder remains about 1.31%, compared with 79.6% for the UINT8 cascade. The monolithic graph measured 14.981 ms median versus 18.235 ms for four submissions.
- The combined QNN context caches the two frontend graphs, monolithic encoder, and nine-node decoder K/V graph in a 19,890,176-byte payload. A representative fresh process restored all four graphs in about 52 ms without rebuilding nodes. The computed frontend stays below 1 ppm squared relative L2, and its WAV-derived encoder result is 192 ppm versus 172 ppm for the precomputed-input baseline. Cache I/O and descriptor reconstruction remain freestanding C through Kernel32 and direct QNN calls.
- The plain-C autoregressive decoder maintains per-layer self-attention KV caches, applies Whisper suppression rules, and decodes token bytes without libc. The shared task pool uses 12 persistent workers on the test machine for cross-attention heads, vocabulary ranges, and cache preparation. Conventional cross-attention K/V caching plus FP32 vector accumulation reduced CPU decoder time from 5.351 seconds to 1.063 seconds; the cached HTP K/V graph then reduced normalization and projection preparation from about 56.5 ms to a median 14.6 ms including CPU import. Three HTP-assisted runs had a 1.170-second median decoder time and preserved all 109 generated tokens exactly.
- The 6,420.011-second Bundestag hearing was converted to a 410,880,776-byte mono 16 kHz `pcm_f32le` WAV and transcribed as 257 overlapping windows. The repaired corpus contains 27,435 generated tokens, no capped or sustained-repeat windows, and removed 1,705 duplicated boundary words. Aggregate measured stage time was about 326.9 seconds; cross-attention accounted for 33.4%, feed-forward layers 23.8%, vocabulary logits 14.6%, log-mel computation 12.3%, and self-attention 9.2%.
- Persistent native batching produced byte-identical transcripts and token counts across a ten-window comparison. One process took 15.172 seconds versus 19.324 seconds for ten processes, reducing probe wall time by 21.5%. Within token generation, retain correctness-first full-vocabulary selection; evaluate FP16 cross-attention cache storage and a persistent full-layer QNN graph against the measured CPU path before adopting either.
- LLVM's PE audit reports ARM64, no exception or CLR runtime tables, and imports confined to `KERNEL32.dll`; `QnnHtp.dll` and the optional KernelBase wait/wake entry points remain dynamically loaded.

This isolates the D3D12 failure to the installed Windows D3D12/runtime and Qualcomm driver path rather than the direct QNN path. Re-run both probes after OS or NPU driver updates.

Direct QNN is a separate working path and does not depend on D3D12 device creation for the Hexagon adapter. FP16 is now the primary wider-precision path; advanced UINT8 calibration remains the fallback if complete-layer FP16 latency or memory is unacceptable.