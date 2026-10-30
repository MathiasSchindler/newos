# Nemotron M2: native streaming frontend and runtime artifact reader

M2 is implemented and accepted on the Windows ARM64 Snapdragon target.
This is a WAV-to-feature frontend and artifact validation milestone, **not
yet a native ASR recognizer**. The provisional M1 v17 model package and its
unchanged 320-ms final latency goal are preserved.

## Accepted evidence

`D:\newos-data\nemotron-asr\m2\acceptance-final-2\report.json` records:

- All16 frozen M0 fixtures, including the 65-chunk long case, with three
  audio supply patterns: `5120`, `1,159,256,4096` and `173,8192,37`.
- 48 native ARM64 runs and 5,216,256 compared feature values: zero violations
  under the original `abs(error) <= 2e-5 + 2e-5 * abs(reference)` gate.
  Maximum absolute error was 6.67572021484375e-5; the combined absolute/
  relative criterion, not absolute error alone, determines acceptance.
- Byte-identical native feature traces across supply patterns.
- A fixed512-sample ring and 30,816-byte frontend state. Reset parity, silence,
  tail padding, exact valid-frame accounting and first-chunk readiness at
  4,096 samples are additionally covered by C development tests.
- All16 runtime resources hash-verified, including real graph/cache/decoder
  contracts. Wrong operator metadata is rejected before restore.
- Unicode WAV, package, model-config/tokenizer and output paths; a Unicode
  deployment directory with only Windows System32 in PATH.
- 15 native negative-path checks, six C-reference test suites with malformed
  WAV/package and allocation-failure tests, and 231 existing native reader
  checks. The related29 M0/M1/package-binding Python tests also passed.
- Clean confined to the existing isolated Nemotron build, byte-identical
  rebuild, restored package validation, and unchanged approved M1 manifest.
- ARM64 PE code with Kernel32-only imports; no CRT, CLR, external inference
  framework or unwind runtime.

## Frontend contract

[The portable frontend](../../src/apps/nemotron/nemotron_frontend.c) uses
newos [math](../../../../src/shared/math/math.h) and runtime support. It retains
the previous raw sample for FP32 preemphasis0.97 and bounded centered STFT
history. The frozen M0 symmetric400-sample Hann window is centered in a
512-point FFT; Slaney128-mel coefficients are copied exactly from M0, not
regenerated using a different library. Power, natural log plus2^-24 guard,
no eval dither and no normalization match the selected reference.

Exactly `floor(real_audio_samples / 160)` valid frames are emitted.
First/steady model chunks have25/32 frames. Only a final partial feature
chunk is zero-padded; waveform EOF padding is not new input audio.
Full chunks are emitted immediately. A separate END callback handles EOF
after an already emitted full chunk, without a duplicate or delayed chunk.
The diagnostic trace writer marks its last record FINAL.
Reset restores initial zeros, preemphasis, frame and chunk counters.
Consumer failures, nonfinite or unnormalized input, reuse after EOF, and
audio shorter than one valid frame are explicit errors.

[The portable WAV reader](../../src/apps/nemotron/nemotron_wav.c) accepts
16-kHz mono PCM16 RIFF/WAVE, validates complete RIFF/chunk extents and format
fields, skips bounded ancillary chunks and rejects duplicate/missing data
or format chunks. It does not load the whole recording. Windows file access
and strict UTF8-to-UTF16 conversion remain in
[the Windows adapter](../../src/apps/nemotron/nemotron_windows.c).

This frontend currently executes on CPU. Model inference is not part of M2;
no CPU encoder fallback or new GPU/HMX placement is implied. Future frontend
offload needs an independently measured benefit.

## Runtime package and trace formats

[The existing artifact reader](../../src/apps/nemotron/nemotron_reader.c)
now supports kind5 runtime indexes in the same `NM1PKG01` envelope. Legacy
graph/context/fixture/result formats remain supported. The `NMRT3201`
payload fixes the model/profile, language, frontend constants, decoder IDs,
v17 host/DSP/catalog/OEM-driver identities and tokenizer identity.
It contains16 ordered resource descriptors, a padded512-float window and
128-by-257 FP32 mel filters. Resource paths are bounded UTF8 relative paths;
only the pinned installed OEM driver uses an absolute path. Invalid offsets,
unsupported profiles, traversal, hashes and identities are rejected.

The index is139,520 bytes and references existing assets under its directory,
avoiding duplication of multi-gigabyte graphs. Keep the index in its declared
resource root. The pinned M0 contract is a bound resource; actual graph IO,
all24 encoder caches, subsampling caches, two-layer prediction states and
joiner shapes are validated. FP32 operator metadata is checked against
graph tensor dtype/rank/dimensions without executing model math.

`check` verifies all resources before model restore. `features` validates
the index and frozen frontend tables but does not load unused model graphs.
Output `NM2FEAT1` traces contain a128-byte provenance/checksum header and
per-chunk index/flags/width/valid-length records plus FP32 values.
Output files must not exist; failed generation removes only its own partial
output. The independent Python auditor checks shape, padding, counts,
provenance and checksums before numerical comparison.

## Build and reproduce

From the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\experimental\snapdragon\tools\nemotron\test-m2.ps1
```

[The M2 gate script](../../tools/nemotron/test-m2.ps1) builds the native
frontend, runs development/negative and full-corpus tests, and performs
isolated clean/rebuild/restore. Its developer Python/DLL are verification
tools only and are not required by the native executable.
The ordinary [probe build](../../tools/nemotron/build.ps1) also builds
`build\nemotron-asr\nemotron-frontend.exe`; default all-app builds are unchanged.

```powershell
$Data = 'D:\newos-data\nemotron-asr'
$Frontend = '.\experimental\snapdragon\build\nemotron-asr\nemotron-frontend.exe'
& $Frontend check "$Data\nemotron-runtime-m2.nmp"
& $Frontend features "$Data\nemotron-runtime-m2.nmp" `
  "$Data\corpus\silence.wav" "$Data\silence-new.nmfe" '1,159,256,4096'
```

[The exporter/auditor](../../tools/nemotron/nemotron_m2.py) creates the index
from pinned M0 tables and the approved M1 v17 resources. No network, DSP
resigning, certificate, driver, boot, trust or machine execution-policy
changes are required. The PowerShell override above is process-local.

[M3](nemotron-m3.md) now separately connects this frontend to DSP model
execution, greedy RNN-T, tokenizer/text output and full WAV ASR acceptance.
