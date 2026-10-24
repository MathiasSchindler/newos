# Nemotron M5: microphone capture and energy endpointing

M5 adds Windows WASAPI live input to the accepted M4 FP32 DSP pipeline.
Implementation and native fault tests are complete; the required30-minute
physical-microphone acceptance remains blocked by the memory-growth gate.
The first attempt failed after
807.820 seconds of capture when the unchanged5.12-second queue filled.
Its evidence remains under `D:\newos-data\nemotron-asr\m5\live-30min-1`.
Memory stayed approximately5.138GB, but that is not a successful live gate.
The endpoint-policy retry captured1289.923 seconds with queue peak18560
samples and stable memory, then stopped on a Windows data-discontinuity flag.
That evidence is retained under `m5\acceptance-final-3`; no missing samples
were ignored and no aborted run was accepted.
The MMCSS/source-continuity retry under `m5\acceptance-final-4` completed
1800.009 seconds of capture without audio failure, cancellation or source gaps.
All28800160 samples were consumed and accounted for; queue peak was14880
of81920 samples (0.930 seconds), the final queue was empty, and21 utterances
completed. Only statistics and noncontent activity were retained.
However, first/last steady-quarter working-set growth was7373371.535 bytes
(approximately7.03MiB), exceeding the unchanged2MiB allowance.
Final working-set growth from the first steady measurement was6971392 bytes
(approximately6.65MiB). The strict auditor therefore rejected the run;
there is no M5 acceptance report and clean/rebuild was not reached in that run.
The memory rise coincides with the first completed utterance around991 seconds,
after a long idle interval, and then largely plateaus. This is consistent with
first-use residency, but does not prove its cause or rule out a leak.
Memory initialization/residency needs investigation before another acceptance
attempt; the threshold and warmup have not been loosened.

## Capture, placement and bounded state

[The Windows adapter](../../src/apps/nemotron/nemotron_capture.c) loads
Windows Ole32 from System32 and uses the default console capture device,
shared-mode event-driven WASAPI and a100-ms buffer. There is no third-party
audio DLL, Windows speech-recognition service or CPU model fallback.
The native PE still statically imports only Kernel32 and has no CRT, CLR or
unwind runtime. Ole32/MMDevAPI are Windows system components, not freestanding
replacement audio services.
The capture participant now registers Windows MMCSS `Audio`/HIGH through
System32 Avrt and restores that registration at shutdown. This is a
thread-scoped standard audio API, not a machine scheduling-policy change.
Device source-frame positions are checked independently of packet flags;
gaps/reversals are fatal and quantified. Discontinuities remain fatal even
when scheduling is improved.

Supported mix formats are PCM16 or IEEE float32,1..8 channels, and
8/16/24/32/44.1/48/88.2/96kHz. Channels are averaged to mono.
[The portable converter](../../src/apps/nemotron/nemotron_audio.c) uses a
causal64-tap Blackman-windowed sinc polyphase filter to16kHz;16kHz input
bypasses it. Persistent phase/history makes output bit-identical across
input partitions. Output count is floor(inputFrames*16000/inputRate).
The causal filter delays audio by approximately32 input samples; there is no
future-sample padding or raw recording. Nonfinite samples and unsupported
formats are explicit failures; finite clipping is counted.

[The live driver](../../src/apps/nemotron/nemotron_live.c) uses the existing
two-participant project task pool. Inference stays on the driver-owning
main thread; capture runs separately. Its SPSC ring holds81920 float samples,
or5.12 seconds. Producer overflow stops the run; it does not enlarge the ring,
slow hardware, silently discard audio or overwrite the first failure reason.
Ctrl-C stops capture, drains accepted audio, finalizes and shuts down the pool.
Runtime/device-loss, privacy-denial, timestamp and later data-discontinuity
errors are visible. Only a first-packet startup discontinuity is counted
without pretending earlier audio was captured.

Encoder, conditioning, prediction and joiner remain on the unchanged signed
v17 Hexagon/HVX FP32 backend. CPU work is capture/downmix/resampling, energy
endpointing, the validated frontend, cache carry, greedy control and events.
GPU/HMX use, on-chip caches, new signing identities and final realtime
performance are not claimed.

## Endpoint policy

The detector uses10-ms RMS frames, a0.005 normalized-PCM threshold, three
consecutive voiced frames and300-ms preroll. This is an energy detector,
not a neural speech/noise classifier: loud background sounds can trigger it.
Endpoint silence is configurable100..10000ms, default800ms.

The user explicitly approved buffering tentative silence to avoid spending
DSP time recognizing a confirmed endpoint's entire silence interval:

- If speech resumes before the endpoint, the complete internal pause is
  fed to the model byte-for-byte, in order, before resumed speech.
- At a confirmed endpoint, retain up to200ms of trailing context and
  explicitly count the remaining silence as `endpoint_trimmed_samples`.
  The endpoint decision still waits for the configured silence interval.
- EOF/cancellation before a confirmed endpoint retains all tentative audio.
- New utterances reset the frontend, encoder caches and prediction/decoder.
  The WAV path and frozen M0 audio/numerical contracts do not change.

The silence buffer is fixed at160000 samples for the maximum10-second setting.
Every cleanly consumed sample is accounted for as model input, deliberately
trimmed endpoint silence or idle audio. Trimming is segmentation, not a
capture-queue drop or a change to the speech threshold.

## CLI and privacy

```powershell
$Data = 'D:\newos-data\nemotron-asr'
& .\experimental\snapdragon\build\nemotron-asr\nemotron-asr.exe `
  --package "$Data\nemotron-runtime-m4.nmp" --listen `
  --duration-s 1800 --endpoint-ms 800 --quiet-transcripts `
  --output "$Data\new-live-activity.jsonl" --stats "$Data\new-live-stats.json"
```

Without `--quiet-transcripts`, live mode emits normal partial/final UTF8
transcripts to standard output or the explicitly selected new output file.
No mode records raw audio. Statistics-only validation must use
`--quiet-transcripts`; its activity schema has no token IDs or text.
Output/statistics files must not already exist.

Live events distinguish `audio_samples`, the absolute end of recognized input,
from `capture_samples`, the detector's consumed capture position.
`endpoint_trimmed_samples` is cumulative. Normal live events retain the M3
sequence/language/model-work fields; quiet activity events contain only
utterance/time/token counts and `transcript_retained:false`.
Stats report hardware format, distinct thread IDs, queue/memory measurements,
failure/cancellation, and `model_samples`, `idle_samples` and trimmed silence.

`--replay-live WAV` feeds a validated16kHz mono PCM16 WAV at wall-clock pace,
repeating it with `--replay-gap-ms` (default10000). It exercises the same
queue/endpoint/model consumer without accessing a microphone.
Live input is mutually exclusive with `--wav`/`--reset-wav`.
An omitted duration listens until canceled; microphone tests are explicitly bounded.

## Tests and acceptance

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\experimental\snapdragon\tools\nemotron\test-m5.ps1
```

[The runner](../../tools/nemotron/test-m5.ps1) builds/audits no-CRT ARM64,
runs231 reader,20 controller and52 audio/capture/endpoint checks,8 evidence
validator tests, and a real two-second cancellation/drain test without DSP.
It then runs two120-second paced short/long speech cases and the full unchanged
M4/M0 WAV token/text/quality/reset/deployment regression, preserves immutable
executable evidence, and verifies byte-identical clean/rebuild.
Without `-Microphone`, its status explicitly leaves microphone acceptance pending.

The physical gate requires explicit consent through `-Microphone`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\experimental\snapdragon\tools\nemotron\test-m5.ps1 -Microphone
```

This captures the default microphone for1800 seconds, retaining only statistics
and noncontent activity events. Run model-backed tests sequentially to avoid
DSP/resource contention. The first failed run remains evidence, not acceptance.

[The live auditor](../../tools/nemotron/nemotron_m5.py) rejects early/canceled
runs, any failure, changed/full queues, incomplete sample accounting, unseparated
threads, retained content, missing measurements and invalid event lifecycle/times.
After a60-second warmup it compares first/last-quarter mean queue and working set:
allow at most100ms queue variation and2MiB memory variation; final working-set
growth from the first steady measurement must also stay within2MiB.
These explicit noise allowances do not relax the320-ms M1 goal or the M6 RTF gate.
A quiet microphone alone does not establish unlimited continuous-speech throughput;
the report scopes actual observed capture and separately records paced speech.
