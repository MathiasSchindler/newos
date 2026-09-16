# GLM-OCR: freestanding C on Snapdragon HTP

## Contract

Third independent experiment beside Whisper and TranslateGemma. Production code
is statically linked freestanding C with no libc/CRT, using Windows system APIs
and QNN only. No Ollama, Python, Paddle, PyTorch, OpenCV or ONNX Runtime in the
production path. Offline conversion and reference generation may use development
tools; ordinary builds consume their immutable outputs.

Keep learned computation on the NPU where supported and minimize CPU work,
submissions and transfers. Measure end-to-end latency and CPU time at preserved
recognition quality; more NPU operations alone do not prove a speedup. Never
silently substitute CPU inference for unsupported HTP operations. Original
weights and the existing Whisper/TranslateGemma deployments remain untouched.

## Stage 1: source and native verification (complete)

- Source: https://huggingface.co/zai-org/GLM-OCR
- Revision: `2e85a62840ccac27daa451df36c736c4636b8628`.
- Original model: `models/glm-ocr/`, relative to `experimental/snapdragon/`.
- Tracked source catalog: `tools/glm-ocr-model.json`.
- Weight file: 2,650,579,464 bytes; SHA-256
  `a16eb0de98d199293371c560f95f83130d2a2c9612449df16839f08ff9498815`.
- Inventory: 526 BF16 tensors, 1,325,258,240 stored elements,
  2,650,516,480 payload bytes, 62,976 JSON header bytes.

The stored-element count differs from the advertised 0.9B figure. The Stage 2
reference audit below explains the unused MTP group, but does not reconcile the
remaining count with that marketing figure. All original tensors are retained
without quantization or conversion.

The pinned model card declares MIT for GLM-OCR. This source revision has no
standalone LICENSE file: its original README is preserved and hash-pinned.
Before redistribution, preserve the applicable full license and notices as well.
The separate PP-DocLayoutV3 model used by the upstream document pipeline is
Apache-2.0 and is not downloaded or implemented in Stage 1. Model-only operation
must not be presented as equivalent to that complete pipeline's benchmark.

### Commands

From the repository root in Windows PowerShell:

```powershell
.\experimental\snapdragon\tools\build-ocr.ps1
.\experimental\snapdragon\tools\build-ocr.ps1 -Test
.\experimental\snapdragon\tools\build-ocr.ps1 -Download -Verify
.\experimental\snapdragon\tools\build-ocr.ps1 -Test -Verify
```

VS Code provides corresponding GLM-OCR build, tests, download and offline
verification tasks. They use process-scoped ExecutionPolicy Bypass; no persistent
policy change is needed. Requires Clang/LLVM (tested with Clang 22) and Windows
ARM64. Download additionally uses Windows curl and HTTPS, without credentials.

`build/ocr/ocr-model.exe` imports Kernel32 only, links the existing SHA-256 code
and ARM64 stack probe, and passes an automated no-CRT PE audit. Examples:

```powershell
.\experimental\snapdragon\build\ocr\ocr-model.exe --self-test
.\experimental\snapdragon\build\ocr\ocr-model.exe --weights .\experimental\snapdragon\models\glm-ocr\model.safetensors a16eb0de98d199293371c560f95f83130d2a2c9612449df16839f08ff9498815
```

`--verify FILE SHA256` verifies any file. `--weights FILE SHA256` additionally
checks the bounded little-endian safetensors header length. Exit codes: 0 success,
1 I/O or verification failure, 2 invalid CLI arguments. Unicode paths are supported.
Streaming uses a fixed 1 MiB buffer and 64-bit lengths, not a full weight copy.

The PowerShell development path checks pinned Git blob identities for small
source files, uses the native verifier for SHA-256, and writes `source-lock.json`
and `tensor-audit.json`. Safetensors JSON is parsed by a structured JSON parser;
integer dimensions, dtype widths, offsets, exact payload coverage and the pinned
inventory totals are checked. Tokenizer JSON is retained byte-for-byte, not parsed
through PowerShell's case-insensitive object model. Native code does NOT yet parse
tensor JSON or validate the complete model graph. Hashes bind files to the catalog;
they are not publisher signatures.

Downloads resume in `models/glm-ocr.partial/`, guarded against concurrent download
writers. Only a verified complete directory is promoted. Existing model source
files are verified, never overwritten. A corrupted completed file fails closed;
inspect it before explicitly moving it aside and retrying. `-Verify` is offline
and recomputes verification reports; it never fetches a newer revision.

Validation: SHA-256 empty and split-ABC known answers; 46 regression checks include
independent .NET hash comparisons across block and 1 MiB read boundaries, Unicode
paths, bad/missing hashes and files, header bounds, tensor shape mismatches,
unsupported dtypes, gaps, overlaps, unclaimed payload and malformed JSON.

## Stage 2: tokenizer and initial execution contract (complete)

Implemented tool-private C in `src/tools/ocr/ocr_tokenizer.{h,c}`:

- Byte-Level BPE, no normalization, source regex partitioning, `ignore_merges=true`.
   Whole-piece vocabulary lookup precedes ranked merges; equal ranks choose the
   leftmost pair. This is not Gemma's tokenizer or byte-fallback decoder.
- 59,282 pieces (59,246 base plus 36 added), 106,026 merge pairs and 831 Unicode
   category ranges extracted using the reference engine itself. Preserve raw bytes
   inside pieces; decode UTF-8 only after concatenation, replacing malformed sequences.
- Added-token matching and source special flags are distinct. In particular,
   `<think>`, `</think>`, image/video placeholders and several other added tokens
   are NOT removed by `skip_special_tokens`. The actual Transformers 5.17 wrapper
   preserves these flags, despite `extra_special_tokens` entries in the config.
- Text, formula and table single-image prompts, with explicit optional no-thinking
   mode; no BOS/EOS auto-insertion. The default prefix is
   `[gMASK]<sop><|user|>\n<|begin_of_image|>`, followed by repeated `<|image|>`,
   `<|end_of_image|>`, the task text and `<|assistant|>\n`.
   Use Jinja `trim_blocks` and `lstrip_blocks`; default Jinja whitespace is wrong.
- Bounded, versioned little-endian artifacts with SHA-256 over header identity and
   payload; native source revision/tokenizer identities, offsets, counts, sorted
   indices, byte alphabet, merge concatenations and range boundaries are checked.
   These checks provide corruption detection/model binding, not publisher signatures.

### Reproduction and validation

Run ordinary native tests with Clang/LLVM only, using existing generated artifacts:

```powershell
.\experimental\snapdragon\tools\build-ocr.ps1 -TestTokenizer -Test
```

This creates `build/ocr/ocr-tokenizer-test.exe` without replacing `ocr-model.exe`.
Native invocation: `--test-tokenizer TABLE FIXTURES`. It retains the ARM64,
Kernel32-only, no-CRT/exception/CLR build audit. No Python, tokenizer library,
Transformers or QNN DLL is loaded by this executable.

Offline regeneration and independent wrapper comparison only:

```powershell
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --export-tokenizer
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --wrapper-check
```

The exporter requires `tokenizers==0.22.2`, `jinja2==3.1.6`; the separate existing
oracle uses `transformers==5.17.0`, `tokenizers==0.23.2`. Neither environment was
modified. Corresponding VS Code process tasks are provided. `--inspect` reports
the source configuration and tensor-role counts. Original small source files are
checked against the pinned catalog before export; this tokenizer-only operation
does not rehash the 2.65 GB weights. Use Stage 1 `-Verify` for weight verification.

Outputs live in ignored `models/glm-ocr-tokenizer-v2/`: `tokenizer.got`,
`tokenizer-fixtures.got`, `manifest.json`, and separate `wrapper-check.json`.
The manifest records source/exporter hashes, versions, table statistics and prompt
examples. Export uses a staging directory; differing artifacts in an existing
destination are rejected, not overwritten. `--output` selects a fresh destination.
Earlier ignored `glm-ocr-stage2` and `glm-ocr-tokenizer-v1` directories are diagnostic
exports with superseded prompt/flag assumptions, not supported inputs.

Validation on Windows ARM64: **180,712 oracle fixtures**, also compared independently
with Transformers 5.17; **35 negative native checks**; previous **46 verifier
regressions**. Fixtures include every vocabulary ID in both decoder modes, valid
UTF-8 vocabulary pieces for encoding, multilingual and random inputs, malformed
decoded bytes, ASCII controls, contractions, whitespace, long input and 30 prompt
combinations. Output-shortage checks accompany every nonempty applicable fixture.
Negative cases include invalid UTF-8, invalid IDs/tasks/counts, truncated/corrupt
artifacts, wrong source identity and malformed tables with recomputed hashes.

### Runtime bounds

The C API accepts at most 8,192 UTF-8 input bytes or decoder token IDs. Decode
concatenation is capped at 32 KiB, then writes caller-bounded UTF-8 output. Return
`-1` means invalid input or insufficient capacity; discard partial output. Empty
input succeeds. Table storage must stay alive and immutable for the tokenizer's
lifetime. Scratch arrays are per-call stack storage; there is no allocation or
global mutable tokenizer state. The diagnostic runner's large static fixture
buffers are test-only, not the intended inference memory design.

The BPE implementation deliberately starts with a bounded quadratic merge scan;
it is a correctness baseline, not a tokenizer throughput claim. Prompts directly
emit verified IDs, avoiding this scan in the intended fixed-task OCR path.
General chat/multi-image/video prompts, streaming UTF-8 output and model execution
are not implemented. Vocabulary IDs 59282..59391 have no tokenizer piece and are
rejected, even though the model's embedding/head dimension is 59392.

### Reference forward contract

Audit reference: Hugging Face Transformers release **v5.17.0**, particularly
[modeling_glm_ocr.py](https://github.com/huggingface/transformers/blob/v5.17.0/src/transformers/models/glm_ocr/modeling_glm_ocr.py)
and [processing_glm46v.py](https://github.com/huggingface/transformers/blob/v5.17.0/src/transformers/models/glm46v/processing_glm46v.py).
This is a source-code audit, NOT a numerical forward comparison or a separately
hash-pinned downloaded reference tree. Pin that tree with the image/oracle stage.

Stored element roles from the original header:

| Role | Elements |
| --- | ---: |
| Text layers 0..15, embedding, norm and head | 673,285,632 |
| Vision and connector | 434,120,192 |
| Unused `model.language_model.layers.16.*` MTP group | 217,852,416 |
| Ordinary forward groups combined | 1,107,405,824 |

The reference constructs 16 text layers and explicitly ignores unexpected layer
16 weights. This does not authorize deleting the original MTP tensors.

- Text: hidden 1536, intermediate 4608, 16 query heads, 8 KV heads, head size 128,
   untied embedding/head, unbiased projections, gated SiLU. Four RMS norms per
   layer: input before attention, post-self-attention before its residual add,
   post-attention before MLP, post-MLP before its residual add. RMS weights multiply
   directly (not Gemma's `1 + weight`), epsilon `1e-5`.
- Text mRoPE uses ADJACENT pairs; temporal/height/width frequency sections are
   `[16,24,24]`, repeated per pair, theta 10000. Vision RoPE instead uses SPLIT
   halves and FP32 rotation arithmetic. Do not share an assumed identical kernel.
- Vision: 24 blocks, hidden 1024, intermediate 4096, 16 heads; biased 3D patch
   convolution `[2,14,14]`, biased QKV/output projections, RMS Q/K head norms and
   gated SiLU. Post-norm followed by spatial 2x2 convolution into 1536 channels.
- Connector: 1536 linear projection, LayerNorm and exact GELU, then gated SiLU
   MLP 1536 -> 4608 -> 1536. LayerNorm is not RMSNorm.
- Single-image placeholders number `grid_t * grid_h * grid_w / 4`, excluding the
   begin/end markers. Image placeholders have modality type 1, surrounding text 0.
   mRoPE positions advance by spatial extent, not by the number of image tokens;
   decode must apply the resulting position delta to physical cache positions.
- Preserve both generation stop IDs **59246 and 59253**. Neither configured
   131072 positions nor processor size fields establish tested deployment limits.
   Image resize/patch ordering and floating-point accuracy remain unvalidated.

## Next stages

1. **Execution contract and tokenizer: completed above.** Initial source audit and
   bounded tokenizer are verified; image and numerical contracts remain below.
2. **Image preprocessing and numerical oracle.** Define BGR/RGB, normalization,
   aspect ratio, patch packing, spatial merging and image-token positions exactly.
   Produce immutable fixtures from real and synthetic images, including narrow,
   rotated and padded shapes. Test existing project image decoders before reuse.
   PDF rasterization is a separate feature, not an implicit external dependency.
3. **HTP capability and precision probes.** Isolated QNN graphs for patch projection,
   vision attention/axial RoPE, merger, text attention/mRoPE, norms, SiLU and KV
   updates. Test real dimensions, finite outputs and cleanup on the installed SDK.
   Source is BF16; FP16 deployment needs a numerical range/error audit. No blind
   cast, silent clamping, assumed BF16 HTP support or premature W4 conversion.
4. **Vision encoder and connector.** Implement and compare every significant tap
   against the oracle. Bound image sizes with tested static buckets; use shared
   buffers and retain the compiled context. No whole-page quality claim yet.
5. **Text prefill and decode.** Dense 16-layer decoder, resident KV, NPU projections
   and selection where correct and useful, incremental UTF-8 output. Start with
   explicit context/output limits; config's 131072 positions is NOT a tested runtime
   capacity. Implement MTP only after a correct ordinary decode baseline.
6. **Usable OCR and quality gates.** Image-to-text CLI first, then tables/formulas
   and optional separately pinned layout analysis. Evaluate German/English printed
   pages, multilingual text, digits, punctuation, omissions, repetition, termination,
   and ordering. Keep a held-out corpus; AI semantic review is allowed and attributed.
7. **Performance.** Matched quality-controlled cold/resident runs: image processing,
   vision, prefill, decode, CPU time, copies, context size and peak memory. Optimize
   batches, buckets, graph fusion and caching using those measurements.

Current status: source acquisition, verification and bounded native tokenizer/prompt
runtime. No image-to-text inference, NPU graph, FP16 accuracy, PDF support or OCR
quality acceptance is claimed. No previous Whisper or TranslateGemma campaign
is restarted.