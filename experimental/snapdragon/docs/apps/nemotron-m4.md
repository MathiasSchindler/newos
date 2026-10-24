# Nemotron M4: model offload, decoder fusion and persistent RPC I/O

M4 is implemented and accepted on the approved Windows-only v17 FP32
Hexagon/HVX backend. Encoder, acoustic conditioning/projection, prediction
and joiner remain DSP model computation; there is no CPU model fallback.
This is not a claim of HMX/tensor-engine use or hardware-maximum throughput.

## Accepted evidence

`D:\newos-data\nemotron-asr\m4\acceptance-final-1\report.json` binds:

- Exact tokens and UTF8 text on all16 frozen M0 fixtures; German WER remains
  8.37696335078534%, zero absolute percentage-point regression.
- Three64-step independent DSP numerical audits at unchanged0.005 absolute/
  relative tolerances:204,800 packed-prediction values,837,632 packed-joiner
  values and1,042,432 fused prediction/joiner values, all zero violations.
  Fused prediction/state targets remain frozen M0; its additional joiner
  reference uses the same pinned CPU model on real captured encoder inputs.
  No M0 fixture/reference was replaced or relaxed.
- All decoder coefficient bits are preserved under packing; malformed
  schema4/fused graph shapes are rejected.20 native controller, alias,
  RPC execution and shared-view checks pass, along with231 reader checks.
- Shared mapped bindings report zero extra host RPC I/O memcpy bytes across
  the corpus, for10,390,023,336 bytes of directly bound I/O. This does not
  include CPU cache-carry copies or imply zero physical memory traffic.
- Fusion reduces actual model RPC calls from4,124 to3,354 across the16 cases:
  770 nonblank prediction/joiner roundtrips removed.
- Reset, relative/Unicode deployment and approved temporary-module staging
  cleanup pass. Clean/rebuild is byte-identical, restored tail recognition
  passes, and the approved M1 manifest/kernel image are unchanged.

## Paired warmed end-to-end comparison

Three independent process pairs alternate M3/M4 order. Each process recognizes
the same WAV twice with reset; the second pass is measured. Timings include
WAV frontend, model, control, I/O and events but exclude package/staging setup.
Both versions use four DSP workers, baseline power, aligned128 coefficients
and the original signed v17 image. The baseline executable is the immutable
M3 accepted copy, not a concurrently rebuilt binary.

| Case | M3 warm median | M4 warm median | Reduction |
| --- | ---: | ---: | ---: |
| German speech `de_de-1586-pcm16` | 15,464 ms | 14,121 ms | 8.7% |
| Silence | 1,400 ms | 1,306 ms | 6.7% |
| 65-chunk long sequence | 24,269 ms | 22,580 ms | 7.0% |

All three medians are nonregressing. Earlier paired trials are retained as
well; environmental/power variation makes one isolated timing insufficient.
These are end-to-end waveform measurements, not a reclassification of the
unmet original320-ms M1 model-only goal or the later M6 realtime/RTF gate.

## Implementation

[The M4 exporter](../../tools/nemotron/nemotron_m4.py) packs immutable FP32
decoder matrix weights into exact32-column tiles using the already supported
metadata-v2 layout. Precision and logical tensor dimensions do not change.
No new DSP image, signing identity, driver, trust or boot change is required.
FP16/fixed-point candidates previously failed frozen M0 gates; unsupported
precision lowering is not adopted merely to report a faster result.

Prediction's two-layer LSTM/projection and the subsequent joiner are combined
as one ordinary graph using existing v17 operators. On nonblank commit,
[the model runtime](../../src/apps/nemotron/nemotron_asr_runtime.c) executes
prediction and joiner for the same encoder frame, commits recurrent state,
and exposes the already computed logits to the next greedy decision.
New-frame/blank-reuse joiner calls remain separate. Cached fused logits are
checked against their encoder frame; logical prediction/joiner counts and
the M3 controller contract are unchanged.

[The transport](../../src/apps/nemotron/fp32/nm_fastrpc_host.c) exposes bounded
persistent mutable workspace views after staging. The runtime binds client
I/O directly to these device-mapped RPC DDR regions. Validated tensor-ID
lookup avoids repeated full graph name scans. Exact view bindings bypass
memcpy; copied diagnostic bindings remain supported, and partial overlap,
wrong dtype/shape or out-of-arena views are explicit errors.
Input/output regions remain distinct; no unproved in-place cache aliasing is used.

`NMRT3204` indexes mark the fused decoder contract and retain the schema2
decode table and original source/module identities. Schema1/2 indexes and
the legacy numerical probe remain compatible. The original M1 v17 and M3
artifacts are not overwritten.

## Remaining host work and next steps

There are no host tensor/model mathematical operations in the accepted path.
The report separately lists necessary host work:

- PCM conversion, preemphasis, STFT, mel and log: the validated CPU frontend.
  Model math was prioritized; frontend offload needs a measured total benefit.
- Greedy argmax/control: the existing signed module has no decoder-loop/
  argmax protocol. A future validated device loop can remove logits traversal.
- Cache carry between disjoint shared regions: still CPU memcpy under the
  no-alias contract. Device-side carry needs explicit order/reset proof.
- UTF8 detokenization, files and user events: host I/O.

Persistent mapped DDR views are **not** claimed to be on-chip SRAM state or
an accelerator-side state-commit API. Firmware/cache flags remain unchanged.
The actual math placement is HVX plus DSP scalar, without GPU or HMX.
Tensor/HMX acceleration, safe precision strategies and further transfer/
frontend optimizations remain measured future opportunities.

## Build and run

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\experimental\snapdragon\tools\nemotron\test-m4.ps1
```

[The acceptance script](../../tools/nemotron/test-m4.ps1) runs numerical,
full quality/offload, paired speed and lifecycle gates. Developer Python is
only for export/verification; the native ASR runtime still has no CRT or
external inference-framework dependency.

```powershell
$Data = 'D:\newos-data\nemotron-asr'
& .\experimental\snapdragon\build\nemotron-asr\nemotron-asr.exe `
  --package "$Data\nemotron-runtime-m4.nmp" `
  --wav "$Data\corpus\short-speech.wav" `
  --language de-DE --chunk-ms 320 `
  --output "$Data\m4-transcript-new.jsonl"
```

Shared RPC I/O is the optimized default. `--copied-io` is a diagnostic control,
not a CPU inference mode; exact tail parity and positive copied-byte counts
are verified. JSONL additionally reports `shared_rpc_io`, `fused_decoder`,
`rpc_graph_calls`, `fused_joiner_reuses`, `host_rpc_copy_bytes` and
`shared_rpc_binding_bytes`.
M5 microphone/endpointing and M6 full live/performance acceptance remain separate.
