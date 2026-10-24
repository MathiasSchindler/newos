# Nemotron ASR: M0 development reference

This is development tooling, not a native recognizer or a QNN deployment.
The executable reference is Transformers, explicitly selected for this
Windows ARM64 bring-up. NeMo checkpoint/configuration and source are pinned
and preserved, but NeMo-to-Transformers numerical and token parity is **not**
claimed. M1 must still establish real HTP feasibility.

## Inputs and storage

[The model catalog](../../tools/nemotron/nemotron-model.json) pins
`nvidia/nemotron-3.5-asr-streaming-0.6b` at
`ea30d66debe3740a08b573244286791d423d6b3e`. It records immutable Git blob
identities for metadata and SHA-256 for both the original NeMo archive and
the FP32 safetensors checkpoint, plus SHA-256 for all metadata, preserved
license material and the four NeMo source files. Fetch materializes SHA-256 for every file
in `checkpoint/source-lock.json`. Downloads resume into `.partial` files
and are promoted only after identity checks. Existing corrupt files fail
explicitly; they are not silently replaced.

The default data root is `D:\newos-data\nemotron-asr`, outside the repository
and the central build/clean tree:

```text
checkpoint/           original weights, tokenizer, model card, license and locks
nemo-source/          four pinned source files and source-lock.json
nemo-config.json      inspected original archive inventory and model configuration
fleurs/               original rights-documented German audio and dataset card
corpus/               PCM16 quality clips and generated diagnostic WAVs
reference-packages/   additional development-only Python dependencies
target/               hardware, drivers, QNN version and PE import inventory
reference-1/          first reference capture, contract and binary tensor fixtures
reference-2/          independent-process repeat
m0-acceptance.json    generated only after successful acceptance checks
```

Keep these inputs/results out of source control. A different data root is
supported by `--data-dir` or the inventory script's `-DataDir`.

[The corpus catalog](../../tools/nemotron/nemotron-corpus.json) fixes German
FLEURS validation rows 0 through 9 at dataset revision
`70bb2e84b976b7e960aa89f1c648e09c59f894dd`, including original transcripts,
asset hashes and CC-BY-4.0 provenance. Suitable existing calibration audio
is reused after hash verification. Otherwise, downloads go to the data root;
temporary signed asset URLs are refreshed and checked against the committed
asset identity, never committed.

Corpus preparation converts float32 to PCM16 with round-to-nearest-even and
saturation. It also produces short speech, an inserted pause, silence,
an exact audio chunk boundary, an incomplete tail, and a repeated speech/pause
sequence spanning at least 64 model chunks. Derived clips are diagnostic,
not independent quality samples. Preserve the FLEURS dataset card and
attribution when sharing them. This ten-clip regression corpus is not the
later command-acceptance corpus.

## Reproduction

Run from the repository root. The existing x64 development Python can be
reused on Windows ARM64; nothing here is linked into the future native tool.
The primary package versions and CPU PyTorch runtime are enforced by the
catalog. Captures also record Python, all installed package versions and
hashes of the relevant upstream implementation files.

```powershell
$Python = '.\experimental\snapdragon\build\gemma-oracle-x64\python.exe'
$Tools = '.\experimental\snapdragon\tools\nemotron'
$Data = 'D:\newos-data\nemotron-asr'
$Deps = "$Data\reference-packages"

python -B "$Tools\nemotron_m0.py" --data-dir $Data fetch
python -B "$Tools\nemotron_m0.py" --data-dir $Data fetch-corpus
python -B "$Tools\nemotron_m0.py" --data-dir $Data corpus

# Install only if the selected reference host lacks the frontend dependencies.
# This command targets the x64 Python used here, not ARM64 Python.
python -m pip install --target $Deps --platform win_amd64 --python-version 3.14 `
    --only-binary=:all: librosa==0.11.0 numpy==2.4.3

& $Python -B -c "import sys; sys.path.insert(0,r'$Deps'); sys.path.insert(0,r'$((Resolve-Path $Tools).Path)'); from pathlib import Path; from nemotron_m0 import inspect_checkpoint; inspect_checkpoint(Path(r'$Data\checkpoint'),Path(r'$Data\nemo-config.json'))"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$Tools\inventory.ps1" -DataDir $Data

& $Python -B "$Tools\nemotron_reference.py" --data-dir $Data --deps-dir $Deps `
    capture --output "$Data\reference-1"
& $Python -B "$Tools\nemotron_reference.py" --data-dir $Data --deps-dir $Deps `
    capture --output "$Data\reference-2"
python -B "$Tools\nemotron_reference.py" --data-dir $Data accept `
    "$Data\reference-1" "$Data\reference-2" --output "$Data\m0-acceptance.json"
```

Check each command's exit code before proceeding. Corpus and capture output
directories must be fresh; the scripts do not erase earlier evidence.
Capture is network-disabled and uses only the verified local checkpoint.
The optional `capture --fixtures silence,tail` filter is useful for bring-up
but cannot satisfy full M0 acceptance.

`pin-corpus` is a one-time maintainer operation for intentionally updating
the committed corpus catalog, not a normal reproduction step. It requires
the expected dataset revision in every selected asset URL.

## Reference and native contract

Every capture produces `contract.json` and named little-endian binary tensors
with dtype, dimensions, byte length and SHA-256 in fixture manifests.

- German is explicit: `de-DE`, prompt ID 9, context `[56,3]`, 320 ms.
- The frontend uses 16 kHz, a symmetric 400-sample Hann window, hop 160,
  FFT 512, 128 Slaney mel bins, preemphasis 0.97, natural log and the upstream
  log guard. There is no feature normalization or evaluation dither.
- The incremental frontend retains the previous sample for preemphasis and
  bounded FFT history. It is checked against full-utterance extraction and
  irregular audio feed partitions, including short inputs and EOF.
- The first model input is `[1,25,128]`; subsequent inputs are `[1,32,128]`.
  Each call emits four encoder frames. The manifest distinguishes fresh audio,
  frontend history and model feature chunks.
- Both pinned checkpoint configurations list lookahead values 3, 0, 6 and 13,
  not 1. The model card's advertised 160-ms mode is therefore not an already
  validated profile of this checkpoint; it needs a separate investigation.
- EOF pads the final feature chunk with zero features, as the pinned
  upstream streaming generator requires, and decodes all resulting frames.
  Valid/padded feature counts are recorded. This is a defined Transformers
  baseline, **not** a claim of NeMo tail equivalence or a padding-free
  endpointing policy. Changing it later requires an explicit new reference.
- Language conditioning is acoustic: concatenate the broadcast 128-slot
  one-hot with 1024-wide encoder output, then the two-layer ReLU projector,
  then the 640-wide encoder/joiner projection. Raw, conditioned and projected
  representations are captured separately.
- Attention/convolution cache inputs are snapshotted before upstream
  in-place mutation; outputs and logical attention length are recorded.
  Their observed dimensions are part of the fixture contract. Native
  fixed-size cache packing is M1 work; native I/O aliasing is not permitted
  without separate validation.
- Prediction is initialized once from blank ID 13087 with zero LSTM state.
  Blank reuses prediction; accepted nonblank commits updated state.
  Prediction states, joiner logits, frame/symbol decisions and token IDs
  are captured. The greedy trace must exactly match upstream `generate`.
- Equal logits select the first index. Nonfinite tensors and reaching the
  ten-symbol frame guard are errors, not successful truncated transcripts.
- The tokenizer's Unigram model, special tokens and actual decoder rules
  are recorded. A naive independent token-ID-to-byte table is insufficient.

The pinned reference uses **SDPA**, not eager attention. During bring-up,
eager attention produced nonfinite first-chunk encoder output for silence;
SDPA passed the finite-output probe. No installed upstream source was patched.
The selected backend is enforced in the model catalog.

Frontend tolerances are tested during capture. Encoder/cache/decoder
tolerances are predeclared M1/M3 comparison budgets, not measured QNN error
bounds. Independent-process reproducibility requires exact tensor hashes,
token IDs and text, including the long sequence.

WER uses Unicode NFC, casefolding, punctuation replaced by spaces and
whitespace tokenization, with no number expansion. Corpus WER is total edit
distance divided by total reference word count, not mean per-clip WER.
Raw punctuation, casing and language tags remain in the captures.

## Verified M0 result (2026-10-13)

The full acceptance command passed for `reference-1` and `reference-2`:

- 16 fixtures, including ten original German quality clips and six diagnostics.
- Long-sequence coverage: 65 consecutive model chunks.
- Exact tensor, cache/state, token and text hashes across independent processes.
- Greedy token parity with upstream `generate` on every fixture.
- Maximum frontend/offline absolute error: `1.9073486328125e-6`.
- German corpus WER: **8.377%**, 16 edits over 191 reference words using the
  normalization above. This is a small regression baseline, not command acceptance.
- 19 targeted tests and the shared project layout/syntax/link checks passed.
- Target: Snapdragon X1E80100, NPU driver `30.0.220.3000`, QAIRT
  `2.50.0.260828`, QNN core `2.39.0`. Vendor static/delay imports were
  inventoried without unresolved dependencies, retaining both available
  FastRPC driver-store candidates rather than guessing the loaded copy.

`D:\newos-data\nemotron-asr\m0-acceptance.json` records the accepted run
identities and target evidence. Each run contains 73,366 tensor files
(approximately 7.602 GiB). Comparison rehashes both sets; this can be a long
filesystem integrity check and is not an inference latency measurement.
NeMo parity, HTP execution and actual loaded DLL selection remain unverified.

## Target inventory and limits

[The inventory script](../../tools/nemotron/inventory.ps1) reuses the shared
hardware/driver inventory and preserves the QNN runtime version lock.
[The PE reader](../../tools/nemotron/nemotron_inventory.py) recursively
inventories static and delay imports of Qualcomm DLLs, including available
FastRPC driver-store candidates. It classifies system/API-set dependencies
and reports unresolved imports as errors.

This does not execute vendor DLLs, determine which of multiple driver-store
copies Windows actually loads, or discover arbitrary conditional
`LoadLibrary` calls. That process audit belongs to the M1 probe. Vendor CRT
imports are recorded separately from the future project's no-CRT PE contract.

## Tests

```powershell
python -B .\experimental\snapdragon\tools\nemotron\test_nemotron_m0.py -v
& $Python -B .\experimental\snapdragon\tools\nemotron\test_nemotron_frontend.py `
    --deps-dir $Deps -v
```

The first suite needs only Python's standard library and covers identities,
malformed/truncated WAVs, format rejection, PCM conversion, tensor dimensions,
WER normalization and static/delay PE parsing. The second needs the pinned
reference packages and covers silence, reset, EOF, bounded history and
frontend partition/offline parity. Full-model captures and acceptance are
separate integration checks.
