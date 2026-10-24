# Nemotron M3: native WAV streaming ASR

M3 is implemented and accepted on Windows ARM64 Snapdragon X. The native
recognizer connects the M2 PCM16 frontend to the approved v17 FP32 DSP model
and emits UTF8 partial/final transcription events. It does not require Python,
PyTorch, NeMo, a developer PATH or an external inference framework at runtime.

## Accepted evidence

`D:\newos-data\nemotron-asr\m3\acceptance-final-1\report.json` records:

- Exact token sequences and UTF8 text on all16 frozen M0 fixtures, including
  silence, boundary, incomplete tail and the65-chunk long sequence.
- German corpus WER8.37696335078534%, equal to M0: zero percentage-point
  regression, within the allowed+1.0 absolute percentage-point limit.
  WER uses the same NFC/casefold/punctuation-to-space/whitespace normalization.
  Text, case, punctuation and UTF8 are separately compared exactly.
- Reset followed by a second utterance matches a fresh process.
- Isolated execution with only Windows System32 in PATH, relative runtime
  resources, and Unicode executable, WAV, model, signed-module and output paths.
  Inputs and model artifacts are local; there is no network retrieval or
  network inference in the application. No machine networking policy changed.
- 10 native CLI negative cases,18 native controller/cache/RPC checks,
  three tokenizer suites with28,224 source-decoder comparisons, six M2
  regression suites and231 existing native reader checks.
- Byte-identical isolated clean/rebuild, restored tail recognition, and
  preservation of the approved M1 manifest and signed DSP kernel.
- ARM64 PE with Kernel32-only static imports, no CRT/CLR/unwind runtime.
  Qualcomm's OEM transport retains its previously documented vendor DLL
  dependency exception.

This does not claim live microphone support, a production distribution,
representative command-corpus quality or final real-time performance acceptance.

## Model execution and controller

[The native model runtime](../../src/apps/nemotron/nemotron_asr_runtime.c)
stages five graphs through the existing signed OEM FastRPC backend:
first subsampling, steady subsampling,24-layer encoder/conditioning/projection,
prediction and joiner. The signed v17 DSP image is unchanged. All model
operations execute on Hexagon HVX or DSP scalar; there is no GPU, HMX or
host model-inference fallback. WAV I/O, frontend, cache/control copies,
token selection and text output are CPU work.

Input/output buffers are separately allocated. Carry copies check dtype,
rank, shape, size and full/partial overlap. Initial subsampling caches and
all24 attention/convolution caches are reset. The leading attention history
is masked until populated, saturating at56 slots.

[The portable greedy RNN-T controller](../../src/apps/nemotron/nemotron_rnnt.c)
initializes prediction once from model blank13087 with zero recurrent state.
Blank advances the encoder frame without another prediction or state commit;
nonblank runs prediction and commits the two LSTM state pairs. Equal logits
choose the lowest token ID. All logits must be finite, the vocabulary shape
must match, and the10-symbol-per-frame protection limit fails explicitly
rather than truncating or returning success-shaped text.

EOF preserves M0's policy: zero-pad a partial fixed-shape feature chunk and
decode all four resulting encoder frames, not only a guessed valid subset.
Frontend END finalizes exactly once, including exact-full-chunk endings.

## Native tokenizer and runtime index

[The native decode table](../../src/apps/nemotron/nemotron_tokenizer.c)
implements the pinned Metaspace decoder: filter special tokens for display,
replace U+2581 with spaces, remove only the initial synthetic space, and
preserve later/doubled spaces and punctuation. No tokenization-space cleanup
is applied. Source decoding is checked for allIDs, random sequences and
all captured M0 transcripts in both special-token modes.

The model vocabulary is13088 with blank13087, while the HF tokenizer has
13089 entries: HF pad13087 and HF blank13088. These distinct contracts are
intentional; model blank never enters the emitted nonblank token sequence.

The `NMRT3202` runtime index embeds the source-hash-bound decode table.
`NMRT3201` M2 indexes remain valid for frontend/resource checks, but ASR
explicitly requires schema2. Graphs and tokenizer JSON remain bound to their
original hashes and relative resource names. The index is347,593 bytes and
does not duplicate the multi-gigabyte model.

## Windows vendor path exception

The pinned OEM FastRPC loader rejects a signed DSP module under a Unicode
directory with status0x80000406, both with percent-encoded and raw UTF8 URIs.
An identical relocated module under an ASCII path loads successfully.

The user explicitly approved the narrow boundary adapter on2026-10-14.
For incompatible module/catalog paths,
[the Windows adapter](../../src/apps/nemotron/nemotron_windows.c) creates an
owned unique temporary ASCII directory, copies only the original signed
module and catalog using CREATE_NEW, verifies their original SHA256s,
and logs the source and staged path. It does not re-sign, alter code bytes,
change certificates/driver/boot/trust policy, or move model computation to CPU.
After closing RPC it removes only its own files and directory. Acceptance
checks actual cleanup. Normal ASCII installations do not need staging.

The process TEMP directory must be ASCII and free of URI-reserved characters
for this vendor boundary; an unsuitable TEMP produces an explicit diagnostic,
not a silent fallback. WAV, runtime index, tokenizer, graph and executable
paths remain Unicode-capable.

## Build, CLI and events

From the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\experimental\snapdragon\tools\nemotron\test-m3.ps1
```

[The gate script](../../tools/nemotron/test-m3.ps1) builds, runs tokenizer,
controller and full WAV tests, then performs clean/rebuild/restore. Developer
Python and reference DLLs are test/export tools only. The execution-policy
override is process-local, not a machine setting.

```powershell
$Data = 'D:\newos-data\nemotron-asr'
& .\experimental\snapdragon\build\nemotron-asr\nemotron-asr.exe `
  --package "$Data\nemotron-runtime-m3.nmp" `
  --wav "$Data\corpus\short-speech.wav" `
  --language de-DE --chunk-ms 320 `
  --output "$Data\transcript-new.jsonl"
```

`--package` and `--wav` are required. `--output` defaults to `-` for UTF8 JSONL
on stdout; diagnostics go to stderr. Files must not already exist.
Only pinned `de-DE` and320-ms `[56,3]` are supported; unknown/duplicate options
and unsupported formats/profiles fail before inference.
`--diagnostics` includes raw special-token-preserving text and DSP profiling.
`--reset-wav SECOND.wav` runs a second recording through a full state reset,
without reloading weights.

Events use `newos.nemotron.asr.v1`, with monotonic `seq`, utterance ID,
real consumed `audio_samples`/`audio_ms`, encoder frame count, accumulated
nonblank IDs and text. Prediction/joiner invocation counts expose blank reuse.
`vendor_ascii_module_staging` reports the boundary exception explicitly.
Audio time is consumed input time, not a promise of word-level timestamps.
Failures have nonzero process status and diagnostics; no successful final
transcription is substituted for an error.

Final events include wall and actual `GetProcessTimes` user/kernel CPU
measurements for WAV frontend/model/I/O/events, excluding package staging.
CPU timer granularity and integer milliseconds can yield zeros on short runs;
that does not prove zero CPU activity. These per-utterance measurements are
not the M1 model-only p95 gate.

The original320-ms M1 target remains formally unmet under its explicit
provisional v17 waiver. [M4](nemotron-m4.md) now implements exact decoder
packing/fusion and persistent shared I/O. Tensor/HMX use, on-device state
commit and further performance work remain explicitly separate.
