# Snapdragon experiments

This directory documents freestanding Windows ARM64 experiments for the Snapdragon X Elite. Probe source and import definitions live under `src/`, scripts under `tools/`, and downloaded or generated model assets under the ignored `models/` directory. The native probe uses no C runtime, SDK headers, or bundled runtime libraries.

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
.\experimental\snapdragon\build\npu_probe.exe .\experimental\snapdragon\build\long-form-35s.wav
```

For Whisper Base, fetch and export its pinned checkpoint, build its independent QNN context, and select it at runtime:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\fetch-whisper-base.ps1
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe .\experimental\snapdragon\tools\export-whisper-decoder.py --model base
.\experimental\snapdragon\build\npu_probe_builder.exe --model=base
.\experimental\snapdragon\build\npu_probe.exe --model=base .\experimental\snapdragon\build\long-form-35s.wav
```

Whisper Small follows the same model-specific path:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\fetch-whisper-small.ps1
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe .\experimental\snapdragon\tools\export-whisper-decoder.py --model small
.\experimental\snapdragon\build\npu_probe_builder.exe --model=small
.\experimental\snapdragon\build\npu_probe.exe --model=small .\experimental\snapdragon\build\long-form-35s.wav
```

Tiny remains the default. Cache files and QNN graph names are model-specific, so Base and Small never restore an incompatible context.

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
.\experimental\snapdragon\build\npu_probe.exe '--single-window=..\data\bundestag-hearing-16k-mono-f32.wav'
```

Use `--quiet` when stdout should contain only the progressively stitched transcript, with no probe headings, timing data, segment markers, or repeated final transcript:

```powershell
.\experimental\snapdragon\build\npu_probe.exe --quiet ..\data\bundestag-hearing-16k-mono-f32.wav
```

The equivalent single-argument form is `--quiet=<path>`. The existing output remains the default for diagnostics and manifest-driven orchestration.

The long-form driver writes one absolute WAV path per line to `window-manifest.txt` and invokes the native batch interface once:

```powershell
.\experimental\snapdragon\build\npu_probe.exe '@C:\path\to\window-manifest.txt'
```

The probe restores the selected model's QNN context, loads its decoder bundle, and starts the `RtTaskPool` once, then processes every manifest entry in order. Structured `WHISPER BATCH SEGMENT` markers let the driver retain individual logs and resume records. Use `-PerWindowProcesses` only when comparing or debugging process isolation.

The external-audio path bypasses the diagnostic graph suite, reports FNV-1a fingerprints for intermediate outputs, and transcribes with a plain-C incremental decoder. German transcription uses the fixed prompt `<|startoftranscript|><|de|><|transcribe|><|notimestamps|>`, descriptor-sized self-attention KV caches, and byte-level token decoding. Tiny retains greedy selection. Base and Small buffer their greedy result and, only when repeated token bigrams and trigrams cross a fixed threshold, try a bounded deterministic temperature schedule and emit the least repetitive candidate. A cached FP16 QNN graph computes encoder normalization and all descriptor-selected cross-attention K/V projections once per audio window. The decoder stores immutable parameters as FP16, widens four lanes at each use site, and accumulates in FP32 without retaining an FP32 shadow copy. It also consumes QNN cross-K/V buffers in place, avoiding a separate FP16-to-FP32 cache import. The persistent task pool parallelizes cross-attention heads, profitable projection rows, and vocabulary ranges; `--decoder-workers=1..32` provides a repeatable partitioning control. Decoder, token, cross-K/V, frontend, and encoder bundles are generated atomically with:

```powershell
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe .\experimental\snapdragon\tools\export-whisper-decoder.py --model tiny
```

The exporter validates checkpoint `config.json` against the pinned model catalog and atomically writes all five bundles. Every production model artifact, including the QNN context cache, starts with the shared 96-byte version-2 header containing model identity, complete dimensions, payload and element types, 64-bit counts and sizes, and an FNV-1a payload hash. Runtime readers reject unsupported versions, wrong models or dimensions, truncation, overflow-sized fields, and hash mismatches before inference.

Python and NumPy remain development-only exporters. Runtime token generation and detokenization use freestanding C without a standard library. Decoder weights, token storage, layer bindings, attention caches, scratch vectors, task-pool state, and profiling belong to an explicit runtime-sized `WhisperDecoder` context; checked 16-byte-aligned arenas replace Tiny-sized global buffers. The native command handles complete compatible WAV tracks and prints a stitched transcript. The PowerShell driver remains useful when compressed-input conversion, resumable per-window records, timestamped output, retry repair, and aggregate profiling are required. Configurable language/task prompts remain separate work.

`npu_probe.exe` is a no-CRT ARM64 PE that imports only `KERNEL32.dll`. It dynamically loads the unavoidable proprietary `QnnHtp.dll` backend, obtains its QNN 2.32 function table, and creates logging, backend, device, profile, and context handles; it does not load ONNX Runtime. Freestanding C parses a fixed 16 kHz WAV and computes Whisper log-mel features. Two cached FP16 QNN graphs run the frontend convolutions, GELUs, and position addition before passing their result directly to a model-generated encoder graph: 88 nodes for Tiny, 132 for Base, or 264 for Small. Runtime activation buffers and temporary builder weights are descriptor-sized; builder weights are released after context restoration. Handles are released in reverse order on success and failure.

The diagnostic path reports peak/current working set and private committed bytes through `GetCurrentProcess` and `K32GetProcessMemoryInfo`, both exported by Kernel32 on the supported Windows target. The Small reference run peaked at 715,628,544 resident bytes and 494,174,208 private committed bytes, below its 768 MiB resident and 512 MiB private budgets. See [snapdragon-x-qnn.md](snapdragon-x-qnn.md) for reusable Windows on Snapdragon and QNN integration findings.

The QNN staging script extracts the Windows ARM64 HTP/System files and their licenses from the pinned `Microsoft.ML.OnnxRuntime.QNN` package into the ignored build directory. The model script pins multilingual `openai/whisper-tiny` revision `169d4a4341b33bc18d8881c4b69c2e104e1cc0af` and verifies the checkpoint's size and SHA-256. Calibration uses one pinned validation clip from each of 16 FLEURS languages. Model and corpus licenses, source revisions, file hashes, quantization errors, calibrated encodings, and deployment artifact hashes are recorded under the ignored `build/` tree. Python and NumPy are development-time preparation tools only; the deployed graph loader remains freestanding C. Review `build/qnn-licenses/Qualcomm_LICENSE.pdf`, the model card, and the FLEURS CC BY 4.0 attribution before redistribution.

The narrow ABI in `../src/qnn_abi.h` was checked against exact QAIRT `2.42.0.251225` QNN core 2.32 headers acquired from Qualcomm's Community SDK archive. Compiler-derived ARM64 sizes and offsets are enforced with static assertions; no offsets are inferred from binaries and no proprietary SDK headers are copied into the repository. The validated provider on this machine is `HTP_QTI_AISW`, backend ID 6, from QAIRT `2.42.0.251225135753_193295`, with QNN core API 2.32.0 and backend API 5.41.0. See [npu-plan.md](npu-plan.md) for the incremental path to a Whisper-like model.

Run the deterministic failure-path suite with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\test-npu-probe.ps1
```

The suite first runs no-CRT ARM64 checks for 18 version-2 artifact contract cases and all four decoder allocation-failure cleanup points. It then builds freestanding ARM64 mock provider DLLs and runs 12 cases covering loader/provider errors, QNN 2.32 compatibility, required functions, reverse cleanup after lifecycle and graph failures, output corruption detection, and successful Add plus Whisper Tiny MatMul execution.

Generate a development-time FP16 encoder-layer bundle with:

```powershell
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe .\experimental\snapdragon\tools\calibrate-whisper-tiny-mlp.py --layer 0 --fp16-only
```

Python and NumPy are used only to verify the pinned inputs and emit raw FP16 artifacts. The deployed `npu_probe.exe` path is freestanding C with no CRT or standard-library dependency; it reads those blobs through Kernel32 and submits the graph directly through QNN.

## Current machine result

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