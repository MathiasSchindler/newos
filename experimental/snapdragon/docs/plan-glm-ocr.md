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
   Stage 3 below validates image resize/packing and single-image positions;
   neural-network floating-point accuracy remains unvalidated.

## Stage 3: RGB preprocessing and position oracle (complete)

The tool-private `src/tools/ocr/ocr_image.{h,c}` now implements the CPU-side
single-image input contract, without OS headers, allocation, libc or external
production libraries. This is an input-processing milestone, not model inference.

- `ocr_image_shape`: aspect-preserving smart resize, factor 28, ties-to-even
   rounding, the reference's minimum-dimension expansion and aspect-ratio check.
   The configured 12544/9633792 limits are **temporal pixel-volume budgets**, not
   edge lengths. Even a single image is budgeted with temporal factor 2.
- `ocr_image_resize`: packed RGB uint8 input with explicit positive row stride,
   antialiased separable Keys bicubic filtering. Double-precision coefficient
   generation, per-axis integer weight precision and uint8 rounding/clamping
   between horizontal and vertical passes match the pinned CPU reference. Plain
   floating bicubic followed by one final rounding is not the same operation.
- `ocr_image_patchify`: CLIP normalization and direct patch output. A 256-value
   lookup per channel avoids repeating normalization arithmetic for each pixel
   and its temporal duplicate. The order is spatial block row/column, local 2x2
   patch row/column, channel, two temporal copies, 14x14 patch pixels.
   Output shape is `[grid_h * grid_w, 1176]`, FP32, and `grid_t = 1`.
- `ocr_image_positions`: exact temporal/height/width positions for one contiguous
   image-token span and surrounding text, modality IDs and mRoPE delta. Integrates
   with all three native task prompts and optional no-thinking mode. A later decode
   step must use physical cache position plus this delta; no KV cache exists yet.

### Build and oracle

Normal build/test requires only Clang/LLVM and the existing generated artifacts:

```powershell
.\experimental\snapdragon\tools\build-ocr.ps1 -TestImages -TestTokenizer -Test
```

This builds `build/ocr/ocr-image-test.exe`; it does not replace the verifier or
tokenizer-only binary. Native invocation is
`--test-images IMAGE_FIXTURES POSITION_FIXTURES`. The existing test reader is reused.
ARM64, Kernel32-only imports, no CRT, no exception/CLR tables remain audited.
`-ffp-contract=off` preserves reference arithmetic; `-fno-math-errno` allows the
geometry square root to use hardware without introducing a libm dependency.

Offline regeneration:

```powershell
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --export-images
```

The image oracle reuses the existing x64 Python 3.14 environment with PyTorch
2.14.0, Transformers 5.17.0 and NumPy 2.4.3. Only missing optional packages were
installed separately under `build/ocr-oracle/`: torchvision 0.29.0 and Pillow
12.3.0. They are made visible only by the image export path. Existing Whisper,
Gemma and calibration environments were not changed. To reproduce this separate
development-only installation using the existing pip:

```powershell
.\experimental\snapdragon\build\calibration-venv\Scripts\python.exe -m pip install --target experimental/snapdragon/build/ocr-oracle --platform win_amd64 --python-version 3.14 --implementation cp --abi cp314 --only-binary=:all: --no-deps torchvision==0.29.0 pillow==12.3.0
```

Do not repeat installation over an existing directory to upgrade it. The exporter
rejects version/source drift. Pinned sources: the actual image processor, backend,
torchvision resize module and GLM-OCR position implementation. PyTorch source
commit is `08187d9e0fba026dc8217405802ab5381dc88d90`; the uint8 filtering contract
was checked against its `aten/src/ATen/native/cpu/UpSampleKernel.cpp`. Hashes are
recorded in the generated manifests. The oracle directly calls the real processor
and position methods, with no model instantiation or weight load.

The document fixtures render original German/English test text using the installed
Windows Segoe UI font; its file hash is recorded, not the font redistributed.
These are synthetic document-like pages, not held-out scans or an OCR quality corpus.

Artifacts: ignored `models/glm-ocr-images-v2/`, containing `image-fixtures.got`,
`position-fixtures.got`, `manifest.json`, `positions-manifest.json`. Image artifacts
bind the pinned preprocessor/model config identities in their hashed header.
The earlier `glm-ocr-images-v1` directory is a superseded diagnostic export.
`--image-output` and build `-ImageDir` select another artifact directory;
`--export-positions` regenerates only the small position fixture set.
Existing differing fixture files are never overwritten. Use the VS Code tasks
`GLM-OCR image oracle export` and `GLM-OCR native image tests` for process-scoped
PowerShell policy bypass without a persistent policy change.

### Verified results

Windows ARM64, Clang 22:

- 1,015 geometry cases, including half-factor rounding, tiny/narrow images,
   aspect-ratio rejection, oversized pages and deterministic random dimensions.
- 34 complete pixel cases: coordinate patterns, random RGB, black/white edges,
   text pages, 90-degree rotation, padding and a 3200x2400 page exceeding the budget.
- **21,652,512 resized RGB channel values byte-identical** to the oracle.
- **43,305,024 FP32 patch values with zero measured absolute error**; regression
   gate is absolute error <= `1e-6`, with NaN/Inf rejected.
- All 34 pixel cases repeated with nonpacked source row strides.
- 42 prompt/mRoPE cases: exact token IDs, three position axes, modality IDs and
   delta, including 180x134 pre-merge grids (6030 image tokens).
- 213 image negative checks plus 168 position negative checks. Existing 180,712
   tokenizer fixtures, 35 tokenizer negative checks and 46 verifier regressions pass.

### Limits and next boundary

Geometry accepts nonzero source dimensions <=10000 and reference aspect ratio
<=200. Resize additionally bounds source area at 16 million pixels. Caller-owned,
nonoverlapping input, horizontal scratch and output buffers must remain valid for
the call. Scratch needs `source_height * target_width * 3` bytes; resized output
needs `target_height * target_width * 3` bytes. Insufficient input, stride or buffer
capacity returns 0. The API does not infer BGR, RGBA, bottom-up images or orientation.

Patchify requires factor-28 dimensions and at most 4,816,896 output pixels. Its
capacity is in **float elements**, requiring `6 * target_height * target_width`.
Positions require one image, no padding/video, <=8192 sequence tokens, `3 * count`
integer positions and `count` modality bytes. These are input API bounds, not
verified model-context capacity. Failure may leave output partial; discard it.

The diagnostic executable reserves large static fixture/output arrays to compare
the entire large page. Production image code has only bounded stack scratch and
caller-owned buffers. It directly writes normalized model-order patches, but
resize remains a scalar correctness baseline with no measured latency claim.
No external image library enters the native path. PNG/JPEG decoding, EXIF handling,
PDF rasterization, scanned-page quality and learned-network tensor taps remain
subsequent work. The isolated HTP graphs below do not run the learned model.

## Stage 4a: isolated HTP primitives (complete)

`src/tools/ocr/ocr_htp.c` builds independent FP16 QNN graphs using the shared ABI,
without Gemma-private code or CPU neural fallback. The installed provider is
`HTP_QTI_AISW`, backend ID 6, core ABI 2.39, QAIRT 2.50.0.260828. Native code
loads the explicitly selected HTP DLL with restricted DLL search flags.
The executable remains ARM64, Kernel32-only static imports, no CRT or
exception/CLR tables; QNN and its platform runtime are loaded dynamically.

```powershell
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --export-htp
.\experimental\snapdragon\tools\build-ocr.ps1 -TestHtp -Test
```

Export is offline development only, using the Stage 3 package versions and pinned
PyTorch commit. Normal native builds consume `models/glm-ocr-htp-v1/htp-fixtures.got`
without Python. `-HtpDir` and `-QnnDir` select fixture/runtime directories; defaults
reuse the existing runtime under `build/`. The VS Code process task is
`GLM-OCR HTP probes`. Outputs are separate `build/ocr/ocr-htp-test.exe`,
`htp-probe.log` and `htp-probe.json`. The report records executable, fixture and
four runtime-file hashes, exit status and timestamp; the log records numerical
errors and accelerator events. Runtime files and original weights are not changed.

The bounded 128-byte hashed fixture envelope binds the source revision and
preprocessor/config identities. Eleven synthetic cases occupy 15,280,632 bytes,
SHA-256 `15ae6d74611f3dbc439a066bdbf9d1acd1222f6643467a4884d547bdd5f472e3`.
Reexport is byte-identical. Reference results are FP32 operations on explicitly
FP16-rounded inputs/constants, NOT on the checkpoint's BF16 tensors.

### Hardware results

All 13 graphs pass three executions each on Snapdragon X Elite. Every execution
requires a positive HTP accelerator cycle/time event (SDK IDs 3003/3004), separate
from host RPC timing. Context/profile/device/backend cleanup returns success.
These profiled smoke runs are not latency benchmarks or proof of HMX utilization.
Nonfatal `DSP_INFO UNSUPPORTED_KEY: 49/50/51` diagnostics remain in the log.

| Graph case | Shape or width | Maximum absolute error, rounded up |
| --- | --- | ---: |
| Two residual additions | 1024, 1536 | 0 |
| Patch projection MatMul | `[4,1176] @ [1176,1024]` | 0 |
| Vision QKV MatMul | `[4,1024] @ [1024,3072]` | 0 |
| Text query MatMul | `[1,1536] @ [1536,2048]` | 0 |
| RMSNorm | 64 | 0.000487 |
| RMSNorm | 128 | 0.000472 |
| RMSNorm | 1024 | 0.000515 |
| RMSNorm | 1536 | 0.000817 |
| Connector LayerNorm | 1536 | 0.000906 |
| SiLU composition | 4608 | 0.004699 |
| GELU | 1536 | 0.007813 |
| Softmax | 16 rows, width 64 | 0.001881 |

Additions use three distinct inputs and require exact output. The eleven oracle
cases repeat their input three times, reset output to NaN before each execution,
and require every output finite with `abs(error) <= 0.003 + 0.005 * abs(reference)`.
Norm cases include constant, very small and large rows with epsilon `1e-5`.
The projections use structured dyadic synthetic weights without bias: their zero
error is NOT evidence that learned projections will be exact.

The first `Sigmoid(x) * x` SiLU graph failed this unchanged numerical gate. Its
replacement `x / (1 + exp(-x))` passes, using HTP Neg, Exp, Add and Divide.
No tolerance was relaxed. Activation fixtures span only `[-8,8]`; this expression
can overflow its FP16 exponential outside that range and is not yet a general
deployment choice. GELU is compared against the exact FP32 reference, not declared
mathematically exact on HTP. Learned activation ranges and accumulated error need
the next numerical oracle before selecting production precision/compositions.

Four negative tests check both exit code and diagnostic: missing fixture, missing
DLL with a valid fixture, corrupted fixture and truncated header. SHA known-answer
tests and the existing 46 verifier regressions also pass. Fixture corruption is
rejected before loading QNN. This is corruption detection, not a signature scheme.

Remaining Stage 4 work: audit original BF16 ranges and candidate FP16 errors;
compare learned tensor taps; probe full attention, RoPE, merger and KV updates.
These primitive results alone do not establish a production precision choice.

## Stage 4b: weight audit and first learned patch tap (complete)

The existing offline exporter now verifies the complete original checkpoint SHA-256
before reading learned tensors. It validates BF16 shapes, exact offset coverage and
the 526-tensor inventory, then audits in bounded 2 MiB blocks. It never writes a
converted checkpoint, clamps weights or removes the unused MTP group.

```powershell
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --audit-precision
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --export-learned-htp
.\experimental\snapdragon\tools\build-ocr.ps1 -TestHtp -HtpDir experimental/snapdragon/models/glm-ocr-learned-htp-v1 -BuildDir experimental/snapdragon/build/ocr-learned
```

VS Code process tasks: `GLM-OCR precision audit`, `GLM-OCR learned patch oracle`
and `GLM-OCR learned patch HTP`. Normal native hardware tests need no Python.
`--precision-output` and `--learned-htp-output` select alternate offline outputs.

### Original weight ranges

`models/glm-ocr-precision-v1/precision-audit.json` records per-tensor and per-role
statistics, source/exporter hashes and NumPy version. The boundary self-test covers
NaN/Inf, overflow, nonzero-to-zero underflow, subnormals and a known rounding error.

| Metric | All stored values |
| --- | ---: |
| BF16 elements | 1,325,258,240 |
| Nonfinite source values / FP16 overflows | 0 / 0 |
| Largest source magnitude | 3.671875 |
| Values changed by FP16 conversion | 545,759 |
| Nonzero values becoming zero | 366,493 |
| FP16 subnormal outputs | 2,083,563 |
| Maximum absolute conversion error | 2.9802322387695312e-8 |
| Sum of squared conversion errors | 1.0353656595289153e-10 |
| Maximum error if subnormal outputs are also flushed | 6.079673767089844e-5 |

Conversion uses IEEE round-to-nearest-even with preserved FP16 subnormals. The
last row is a hypothetical flush-to-zero bound, NOT measured HTP behavior. Small
casting errors do not imply small accumulated network errors or safe activation
ranges. BF16 has fewer significand bits but a wider exponent range than FP16;
these weights fit FP16, but that alone says nothing about intermediate values.

Role accounting stays explicit: text/head 673,285,632 values, vision/connector
434,120,192, unused MTP layer 16 another 217,852,416. Nonzero-to-zero counts are
183,220 / 362 / 182,911 respectively. No source tensors are discarded.

### Learned patch projection

The oracle loads only `model.visual.patch_embed.proj.weight` and `.bias` into the
pinned Transformers `GlmOcrVisionPatchEmbed` module. It invokes that actual Conv3d
forward, not a hand-written reference matrix multiply. A separate flattening
cross-check verifies `[16,1176] @ [1176,1024] + bias` against Conv3d.
The production candidate is a native FP16 HTP MatMul plus bias-add graph.

Three deterministic images cover coordinate RGB patterns, seeded random RGB and
an original German/English text page rendered with Segoe UI. The real image
processor supplies patches; 16 evenly spaced patch rows per image are selected.
Font/RGB hashes and selected indices are recorded. These are development fixtures,
not held-out scans or a full-page encoder test. Native preprocessing parity remains
the separate Stage 3 gate, not an integrated native image-to-patch-to-HTP pipeline.

Each case has two FP32 Conv3d references: original BF16 weight values expanded to
FP32 with unrounded normalized input, and independently FP16-rounded weights/input
expanded to FP32. This is NOT a claim of reproducing a BF16-executed full model.

| Input | Conversion-only maximum error | HTP vs original FP32 | HTP vs candidate FP32 |
| --- | ---: | ---: | ---: |
| Coordinate pattern | 0.000685648 | 0.001609 | 0.001769 |
| Seeded random RGB | 0.000634522 | 0.001793 | 0.001786 |
| Text page | 0.004526854 | 0.004708 | 0.001912 |

Errors are rounded up. All six learned comparisons pass three executions each,
checking every output for finiteness and the unchanged gate
`abs(error) <= 0.003 + 0.005 * abs(reference)`. Including the two residual smoke
graphs, all 24 executions have positive accelerator profile evidence and successful
resource cleanup. Test binaries remain ARM64, Kernel32-only imports, no CRT or
exception/CLR tables. No CPU inference fallback exists in the native probe.

Immutable `models/glm-ocr-learned-htp-v1/htp-fixtures.got` is 15,082,316 bytes,
SHA-256 `37cea26501ec8b7ae3ff922f22b9da86726a539e3421aa268bb891062434321a`.
Envelope kind 6 binds preprocessor/config identities and includes the original
weight SHA-256 in the hashed payload. The native reader enforces that identity,
six records, operation and exact tensor geometry before loading QNN. This detects
corruption and mismatched sources, not forgery by a malicious artifact producer.

The learned probe uses separate `build/ocr-learned/` logs, report and executable,
preserving the original synthetic probe outputs. Six negative tests include missing
files/DLL, corrupt/truncated fixtures, plus wrong weight identity and invalid row
count with recomputed valid envelope hashes. Expected exit codes AND diagnostics
are checked, so a missing DLL cannot mask a fixture-validation defect.

Next: learned vision-block taps including Q/K norms, attention and split-half RoPE,
then merger/connector. Patch success does not validate those layers, activation
ranges, the complete vision encoder, text decoder, OCR quality or performance.

## Stage 4c: initial learned attention validation

The first learned attention branch of vision block 0 now has a diagnostic HTP
graph: norm1, QKV, Q/K RMSNorm, axial split-half RoPE, scaled QK attention,
softmax, context, output projection and first residual. It uses one complete
8x8 patch grid from a deterministic RGB pattern. No MLP or full encoder is run.

`--export-vision-attention` in the existing offline exporter invokes the pinned
Transformers 5.17 attention module with eager attention and captures eleven taps.
Its independently reconstructed context is checked against the module's actual
projection input. Original weights are fully SHA-256 verified before loading.
The two references are original BF16 values expanded to FP32, and the same module
with FP16-rounded weights and input expanded to FP32. The second is NOT an
emulator of HTP kernels or FP16 rounding after every intermediate operation.

Artifacts: `models/glm-ocr-attention-v1/htp-fixtures.got`, 15,378,880 bytes,
SHA-256 `1660d931b593aa0545a909eecd7db3dd103affb6f46def5c0719a72b94a4144b`.
Kind 7 binds the source weight hash and exact geometry. The original failing run
is preserved under `build/ocr-attention/`; corrected runs use
`build/ocr-attention-precision/`. Tasks: `GLM-OCR vision attention oracle` and
`GLM-OCR vision attention HTP`. Six artifact negative checks and native no-CRT
build audits pass. The historical failure and its verified fix are described below.

With the original native Softmax node, all eleven taps passed against the
FP16-input/weight candidate reference. Against the original reference, the first
ten taps passed but two of 65,536 residual values failed the unchanged
`0.003 + 0.005 * abs(reference)` bound, reproducibly over three executions.
Accelerator profiling and resource cleanup succeeded. Declaring FP32 softmax with
casts did not change the result and was reverted; graph-local HTP precision
compensation also did not change it and has now been removed. FP32 declarations
in the RoPE subgraph do not establish the physical
arithmetic precision used by the optimizer; that remains a separate question.

### Residual failure investigation

Run `GLM-OCR residual investigation`, or offline:

```powershell
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --analyze-attention-failures
```

It consumes the hardware log and immutable oracle fixture, verifies fixture and
executable hashes against the run report, matches logged reference bits exactly,
and writes `build/ocr-attention/residual-analysis.json`. The native diagnostic
logs both failing operands and their context rows without changing graph math.
Only the offline analyzer recomputes the two projection dot products in FP64.
The analysis succeeds when the two failures are reproduced and their signed error
decomposition closes; it does not convert the failing hardware gate into a pass.

Indices are zero-based, flattened as `patch * 1024 + channel`:

| Metric | Index 2471 (patch 2, channel 423) | Index 19669 (patch 19, channel 213) |
| --- | ---: | ---: |
| HTP input | 1.8857421875 | 1.79296875 |
| HTP projection | -1.94921875 | -1.7666015625 |
| Original residual | -0.0668313503 | 0.0232105255 |
| HTP residual | -0.0634765625 | 0.0263671875 |
| Absolute error | 0.0033547878 | 0.0031566620 |
| Allowed error | 0.0033341567 | 0.0031160526 |
| Excess over bound | 0.0000206311 | 0.0000406094 |
| Cancellation factor | 57.42 | 153.49 |

The cancellation factor is the sum of the absolute original operands divided by
the absolute original residual. Cancellation reduces the output magnitude and
thus the relative part of the error budget; it does not create a new arithmetic
error in these additions. The HTP residual is exactly the sum of the two observed
FP16 operands in BOTH cases. Promoting just that addition cannot recover lost
input or projection information.

Signed error contributions, summed to `HTP residual - original residual`:

| Contribution | Index 2471 | Index 19669 |
| --- | ---: | ---: |
| Input conversion, including original-add roundoff | +0.0003877878 | +0.0001173019 |
| Projection shift from input/weight conversion in FP32 oracle | +0.0000203848 | -0.0000147820 |
| Propagated context error through fixed candidate weights | +0.0025443705 | +0.0026717248 |
| HTP projection versus FP64 dot on observed context | +0.0004021755 | +0.0003823291 |
| Candidate FP32 oracle dot roundoff | +0.0000000693 | +0.0000000881 |
| Final HTP addition | 0 | 0 |

The dominant contribution is the already-perturbed attention context, followed by
projection execution/output rounding and input rounding. The context term combines
all upstream numerical differences; it does not identify softmax, RoPE, norms or
QKV individually as the cause. The projection term includes accumulation, bias
and output representation effects, not just one known kernel's rounding mode.
Original input is reconstructed from original FP32 residual minus projection, so
the input term explicitly includes original-add roundoff.

Counterfactual arithmetic with the original input but unchanged HTP projection
would leave errors 0.0029670000 and 0.0030393600, both inside the current bounds.
Using the FP64 projection of observed HTP context with the existing FP16 input
would also bring both inside. These are localization checks, not validated fixes:
neither establishes what an alternative HTP precision configuration will execute.
These observations motivated the following experiments, without relaxing any
tolerance. Full-model acceptance remains open.

### Verified Softmax fix

Promoting the output projection, bias and residual sum to FP32-declared tensors
also produced unchanged results. Those casts were removed. Replacing the native
Softmax node by a stable HTP composition fixed the two failures:

`maximum = ReduceMax(scores)`; `exp = Exp(scores - maximum)`;
`probabilities = exp / ReduceSum(exp)`.

Reductions use the last axis and explicit broadcast reshapes. All operations stay
inside the same HTP graph; there is no CPU neural fallback or intermediate CPU
readback used to compute results. Subtracting the maximum bounds each exponential
input at zero or below. Diagnostic tap readbacks remain for reference comparison.

The input, QKV, norms, RoPE, score computation and output projection are unchanged.
Measured pre-Softmax maximum errors remain unchanged. This isolates the Softmax
implementation path as a contributor to the accumulated context error. It does
not establish which undocumented approximation or rounding strategy the native
HTP Softmax kernel uses. Its earlier output was within the per-tap tolerance but
left insufficient accuracy after projection and cancellation in the residual.

Pattern case, maximum absolute error against original FP32, rounded up:

| Tap | Native Softmax baseline | Stable HTP composition |
| --- | ---: | ---: |
| Probabilities | 0.001909 | 0.000969 |
| Context | 0.008436 | 0.005732 |
| Projection | 0.022313 | 0.009318 |
| Residual | 0.024205 | 0.016055 |
| Residual values outside tolerance | 2 | 0 |

These are maxima over entire tensors, not just the two originally failing values.
The original fixture and both reference arrays remain byte-identical. The gate
still checks every value against `0.003 + 0.005 * abs(reference)`.

The fix also passes two additional full 8x8 grids: seeded random RGB and a small
synthetic text page, each 112x112 pixels. `--attention-case pattern|noise|text|all`
selects export cases. `all` retains the original fixture at the output root and
adds `noise/` and `text/`, each with its own manifest and immutable fixture. The
text case records the Segoe UI font hash; these are development fixtures, not
held-out scan quality evidence.

```powershell
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --export-vision-attention --attention-case all
```

VS Code tasks `GLM-OCR attention corpus oracle` and `GLM-OCR attention corpus HTP`
reproduce all three exports and serial hardware runs. Results are stored under
`build/ocr-attention-precision/`, with `noise/` and `text/` subdirectories. The
single-case `GLM-OCR vision attention HTP` task also uses the corrected directory,
preserving the original failure log, executable and residual-analysis report.

All three cases pass all eleven taps against both oracles for three executions:
15,335,424 value comparisons. Each execution additionally checks all 1,024
probability rows for values in `[0,1]` and sums within `1 +/- 0.003`. Including
the residual smoke graphs, all 27 graph executions provide positive accelerator
profile evidence, with successful cleanup. Six negative tests per case pass.
ARM64, Kernel32-only/no-CRT audits and the existing 46 verifier, 180,712 tokenizer
and complete image/position regressions pass. No speed claim follows from these
profiled, tap-heavy diagnostic graphs; a deployment graph requires separate timing.

Fixture SHA-256 values (each 15,378,880 bytes):
- Pattern: `1660d931b593aa0545a909eecd7db3dd103affb6f46def5c0719a72b94a4144b`.
- Noise: `3e89e48f4b223a573e6775f596a68d30c81988b341f1b2a1509a569546f651de`.
- Text: `4851d7b8b0a2fdab9d47928edbc35f1cdeb678fe6a276c0b34b6041b4adb90a2`.

This stage establishes attention-branch acceptance on three small grids. The
complete first block is covered below; larger grids, more document varieties and
accumulated multi-block precision remain separate gates.

## Stage 4d: complete first vision block

The native diagnostic now runs the complete learned vision block 0 on HTP:
the Stage 4c attention branch, second RMSNorm, gate/up projections, SiLU,
elementwise gating, down projection with bias and the second residual addition.
The seven additional taps are `norm2`, `gate`, `up`, `silu`, `gated`, `down` and
`block_output`, for eighteen taps in total. All neural operations remain inside
one graph; CPU tap readbacks only check results, never supply a neural fallback.

The existing exporter invokes the pinned Transformers `GlmOcrVisionBlock`
forward and captures its actual module inputs/outputs. Exact cross-checks compare
the gated product and final residual to that forward. Both original-value FP32
and independently FP16-rounded input/weight FP32 references are retained; neither
is an emulator of physical HTP arithmetic. The input is the offline original
patch-embedding output rounded to FP16, not an integrated native patch-to-block
pipeline. Original checkpoints and previous attention fixtures are unchanged.

### Stable SiLU and precision

The block computes `factor = exp(min(gate, 0)) / (1 + exp(-abs(gate)))`, then
`silu = gate * factor`. Both exponential inputs are nonpositive, avoiding the
overflow risk of directly evaluating `exp(-gate)` for large negative FP16 values.
The implementation uses HTP elementary operations, not its native Sigmoid kernel.
This establishes an overflow-safe algebraic form, not full-range FP16 accuracy:
the learned gate ranges tested here are listed below.

An initial equivalent ordering, `(gate * exp(min(gate, 0))) / denominator`, left
one final residual outside the original-reference tolerance on the text case:
index 39446 (patch 38, channel 534). The final addition was exact for its observed
FP16 operands, so promoting only that addition could not recover the lost values.
Replacing down MatMul/bias with FullyConnected gave identical failing bits;
an explicit FP32-declared second RMSNorm introduced additional failures. Both
experiments were removed. Computing the stable factor before multiplying the gate
fixed the remaining failure, with no reference or tolerance changes. This measures
an operation-order effect; it does not identify undocumented HTP rounding rules.

| Case | Original gate min / max | Block output max error vs original FP32 | vs candidate FP32 |
| --- | ---: | ---: | ---: |
| Pattern | -7.372252 / 7.607652 | 0.021523 | 0.020916 |
| Noise | -4.469445 / 2.846380 | 0.034089 | 0.032616 |
| Text | -6.158003 / 5.352229 | 0.023938 | 0.023354 |

Output errors are rounded up over whole tensors, not just near-zero values.
Every value passes `abs(error) <= 0.003 + 0.005 * abs(reference)` and finiteness
checks. All eighteen taps pass against both oracles over three executions for
each of the three complete 8x8 grids: 37,748,736 comparisons. Probability range
and row-sum checks remain enabled. All 27 executions including residual smokes
have positive accelerator profile evidence and successful resource cleanup.
The binary remains ARM64, Kernel32-only imports, no CRT or exception/CLR tables.
The prior attention corpus and native verifier/tokenizer/image/position gates pass.

### Artifacts and reproduction

Envelope kind 8 contains one operation-9 record, source input 131,072 bytes,
constants 33,618,176 bytes and references 16,777,216 bytes. Native validation
enforces exact geometry and original weight identity before QNN loading. The
diagnostic fixture buffer is bounded at 64 MiB and output scratch at 2,097,152
half values. Twelve negative checks per case cover missing files/DLL, corrupt
or truncated envelopes, wrong source identity, operation, dimensions and lengths,
including recomputed hashes so a missing DLL cannot mask bad structure.

Each immutable fixture is 50,526,656 bytes under `models/glm-ocr-block-v1/`:
- Pattern/root SHA-256: `6bd28ef10138ec4722d637620b883a24ba26d1dede41103c4a0cf3f980328305`.
- Noise SHA-256: `47669a93b5eaa29f3e531fb0c319f70fff6217106daa5cdc14e70175f0a5b709`.
- Text SHA-256: `63643d72d71898145823686793aa58efa906a5c1d4b2c2f15764e593eb907194`.

```powershell
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --export-vision-block --attention-case all
.\experimental\snapdragon\tools\build-ocr.ps1 -TestHtp -HtpDir experimental/snapdragon/models/glm-ocr-block-v1 -BuildDir experimental/snapdragon/build/ocr-block
```

Tasks `GLM-OCR vision block oracle` and `GLM-OCR vision block corpus HTP` reproduce
the three exports and serial hardware tests. `GLM-OCR vision block HTP` checks
the pattern; `GLM-OCR vision block text HTP` is the focused precision regression.
Reports, runtime/executable/fixture hashes and logs use `build/ocr-block/`, with
`noise/` and `text/` subdirectories. `--vision-block-output` selects an alternate
offline destination; `--attention-case` also applies to full-block export.

Next: larger grids, additional documents and accumulated multi-block accuracy,
then spatial merger/connector and text prefill/decode. This is initial block-0
acceptance, not a complete 24-block encoder, image-to-text pipeline or performance
result. Python remains confined to optional offline oracle generation.

## Stage 4e: visible OCR examples and 128-patch bucket

Two readable synthetic examples now accompany the numerical block fixtures:

| Example | Image | Expected transcription |
| --- | --- | --- |
| Receipt: quantities, decimal commas, date, total | [PNG](../models/glm-ocr-examples-v1/receipt/input.png) | [UTF-8 text](../models/glm-ocr-examples-v1/receipt/expected.txt) |
| German: umlauts, sharp s, address, opening times; English greeting | [PNG](../models/glm-ocr-examples-v1/german/input.png) | [UTF-8 text](../models/glm-ocr-examples-v1/german/expected.txt) |

These links refer to locally generated, ignored artifacts. Reproduce them with
the `GLM-OCR examples oracle` task. The original 112x112 text example is also
available after running `GLM-OCR vision block oracle`:
[PNG](../models/glm-ocr-block-v1/text/input.png),
[expected text](../models/glm-ocr-block-v1/text/expected.txt).

The expected text is the rendering input, NOT text recognized by GLM-OCR. There
is still no native image-to-text forward or OCR accuracy score. These fixtures
are not held-out scans, photographs, layout/table/formula benchmarks or evidence
of general OCR quality. They establish a visible starting set for later end-to-end
comparisons. Real scans, camera distortion and full pages remain additional work.

Both new images are 224x112 RGB pixels, drawn with Segoe UI 14, with checked text
bounds. The exporter records the font hash, RGB hash, PNG and expected-text hashes
and provenance in each manifest. It verifies PNG decode against the exact RGB
oracle input and rejects differing existing image/text artifacts. The previews
have been visually inspected for readability and clipping. No font binary is
copied. The processor produces a complete `[1,8,16]` grid: 128 patches, with no
token selection, padding or truncation. Earlier 64-patch binary fixtures remain
byte-identical after regeneration.

The native graph now explicitly separates Q/K/V `[16,tokens,64]`, transposed K
`[16,64,tokens]` and attention matrices `[16,tokens,tokens]`. Tap element counts,
reference offsets, probability rows and RoPE constant lengths derive from the
validated token count. Only 64 and 128 patches are accepted before loading QNN;
this is not an unbounded dynamic-shape implementation. Other grid geometries
with the same count have not been validated by these examples.

The kind-8 envelope limit and native diagnostic fixture buffer are now 80 MiB;
output scratch is bounded at 4,456,448 FP16 elements. For 128 patches, input bytes
are 262,144, constants 33,650,944 and references 35,651,584. Each complete fixture
is 69,564,864 bytes. The native reader checks these lengths from the token count,
the operation, dimensions and original source weight identity. Existing twelve
negative checks per case pass, including rehashed invalid geometry/lengths.

| Case | Block-output max absolute error vs original FP32 | vs candidate FP32 |
| --- | ---: | ---: |
| Receipt | 0.026906 | 0.026036 |
| German | 0.028847 | 0.029892 |

All eighteen taps pass finiteness and the unchanged
`abs(error) <= 0.003 + 0.005 * abs(reference)` gate against both oracles, three
executions per example: 53,477,376 value comparisons. Each run checks 2,048
probability rows for values in `[0,1]` and sums within `1 +/- 0.003`. Both reports
have exit zero, positive accelerator profile evidence and successful cleanup;
ARM64/no-CRT/Kernel32-only audits pass. Previous 64-patch block and attention
corpora, native verifier, tokenizer, image and position regressions also pass.
No neural CPU fallback or tolerance change was introduced. The block input is
still the offline patch embedding, not an integrated native image-to-block path.

Immutable fixture SHA-256 values:
- Receipt: `94e78f8e6cdc5446c2a8d7c9fe67a24c208871da64119db90d724c7baac71819`.
- German: `62d6d8dddd3fc10255629eaf52396159c0924fee6435d17bf2fd0ebc7ccd23b5`.

```powershell
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --export-vision-block --attention-case examples --vision-block-output experimental/snapdragon/models/glm-ocr-examples-v1
.\experimental\snapdragon\tools\build-ocr.ps1 -TestHtp -HtpDir experimental/snapdragon/models/glm-ocr-examples-v1/receipt -BuildDir experimental/snapdragon/build/ocr-examples/receipt
```

`GLM-OCR examples oracle` exports both cases; `GLM-OCR examples HTP` builds and
tests both serially, with separate reports under `build/ocr-examples/receipt/`
and `german/`. Normal native testing consumes pre-generated fixtures without
Python. `--attention-case receipt|german` selects individual examples; `all`
continues to mean the original pattern/noise/text corpus. Next: accumulated
multi-block accuracy, larger document buckets, merger/connector and text decoder.

## Stage 4f: two-block precision investigation (not accepted)

The first two vision blocks now execute as separate HTP graphs. Block 0 runs
three times, then its last FP16 output is copied unchanged into Block 1, which
runs three times on that input. This is not three complete chain executions,
a fused graph, or device-resident zero-copy execution. No oracle tensor replaces
the computed intermediate input and no CPU neural fallback is used.

`GLM-OCR vision chain oracle` generates original and candidate FP32 references
for sequential blocks 0 and 1. The candidate rounds only the initial input and
weights to FP16 before expanding to FP32; it does not round every intermediate.
Kind 9 has two strictly checked records (operations 10 and 11), with no input
payload in the second record. Both records are validated before loading QNN.
Twenty negative checks cover identity, count and both records' geometry/lengths.
The diagnostic fixture buffer and kind-9 limit are 144 MiB; existing limits for
other envelope kinds remain unchanged.

The 64-patch pattern fixture is 100,922,076 bytes, SHA-256
`bf2bcfa9b0c04352c464f17d16ba0e53c82ac806fc053f9ca25e96f09c0f0418`, under
`models/glm-ocr-chain-v1/`. Regeneration remains byte-identical. Block 0 passes,
but Block 1 fails the unchanged `0.003 + 0.005 * abs(reference)` gate:

| Reference | Score failures | Final residual failures |
| --- | ---: | ---: |
| Original FP32 chain | 46 | 2 |
| Candidate FP32 chain | 45 | 1 |
| Candidate weights, exact observed Block-1 input | 21 | 0 |

All other sixteen taps pass in these comparisons. The last row is an offline
conditional oracle, NOT a replacement acceptance gate. It distinguishes local
Block-1 errors from errors inherited from Block 0. The failed FP32-declared
RoPE/QK, explicit Q/K RMSNorm and explicit first RMSNorm experiments have all
been removed. The last had increased original failures to 61 scores/four residuals.

### Reproducible conditional oracle

`GLM-OCR chain capture` invokes `build-ocr.ps1 -TestHtp -CaptureHtp` using separate
`build/ocr-chain-capture/` output. It deliberately still exits 1 for the numerical
failure. The explicit native `--capture-htp DLL FIXTURE DIRECTORY` mode saves each
block's input and final-execution taps to a fresh capture directory. Files use
CREATE_NEW, never overwrite existing files, and are hashed in `htp-probe.json`.
Ordinary `--test-htp` runs do not write tensor captures. No new OS imports or
production dependencies were needed.

Run `GLM-OCR chain boundary analysis` after capture. The existing exporter checks
fixture/envelope, executable and capture hashes and exact geometry, then verifies
the Block-0 output and Block-1 input are bit-identical. It invokes the pinned
Transformers Block 1 with both the actual NPU input and the FP16-rounded ideal
candidate Block-0 output. Conditional weights must match the native fixture bytes.
The result is `build/ocr-chain-capture/chain-boundary-analysis.json`, containing
all eighteen tap summaries, all original failing indices, signed decompositions
and the runtime identities. Analysis success does not mean hardware acceptance.

The signed decomposition closes exactly (maximum residual 0) across all taps:
initial input/weight conversion; ideal handoff FP16 rounding; propagated Block-0
execution error beyond that rounding; local Block-1 execution error. These are
ordered counterfactual differences, not independent causal percentages.

### Score localization

There is no copy/layout corruption at the handoff. Even with the exact observed
input, 21 score failures remain. FP64 dot products on observed post-RoPE Q/K
vectors introduce no tolerance violations relative to the hardware scores.
At the 46 originally failing score indices, the mean absolute signed-component
magnitudes from a telescoping FP64 reconstruction are:

| Local stage | Mean absolute contribution |
| --- | ---: |
| Through QKV, before ideal Q/K norms and RoPE | 0.000786783 |
| Observed Q/K normalization | 0.000764583 |
| Observed RoPE | 0.001945912 |
| QK dot/output rounding | 0.000026204 |

RoPE is the largest of these local contributions at the failing indices. This
does not say QK rounding is negligible everywhere: its whole-tensor RMSE is
0.000819710, and large scores tolerate more absolute error. Local score-component
closure is also checked within 1e-12.

An offline RoPE simulation using FP16 coefficients, FP16 products and FP16 sums
matches 130,952 / 131,072 observed Q/K values bit-for-bit (99.91%). FP32 arithmetic
with only a final FP16 cast matches 102,798 values (78.43%). FP16 coefficients
with FP32 products/sum match 109,542. This strongly supports low-precision
intermediates in the FP32-declared HTP path, but the 120 mismatches preclude an
exact undocumented-kernel claim. A counterfactual using FP32 RoPE followed by
FP16 output and FP64 QK dots reduces original score violations from 46 to 10;
it is not an implemented HTP fix and does not alone validate the full chain.

### Residual localization

| Metric | Index 34755 | Index 48323 |
| --- | ---: | ---: |
| Actual output | 0.1337890625 | 0.0585937500 |
| Original output | 0.1301323175 | 0.0546834469 |
| Absolute error | 0.0036567450 | 0.0039103031 |
| Allowed error | 0.0036506616 | 0.0032734172 |
| Initial input/weight conversion effect | +0.0003089905 | +0.0004169941 |
| Ideal handoff rounding effect | +0.0001223087 | -0.0002267361 |
| Propagated Block-0 execution effect | +0.0010228157 | +0.0016713142 |
| Local Block-1 execution effect | +0.0022026300 | +0.0020487309 |
| Final addition error on observed operands | 0 | 0 |
| Cancellation factor | 25.13 | 99.23 |

The final sums are exactly `1.7021484375 - 1.568359375` and
`2.7421875 - 2.68359375`. Cancellation reduces the reference magnitude and hence
the relative tolerance budget; it creates no new arithmetic error at these
two additions. Higher precision only at the last addition cannot recover earlier
errors. Initial conversion and handoff representation alone are not the dominant
terms. Block-0 and local Block-1 errors reinforce each other here.

### Measured HTP RoPE precision experiments

Three opt-in experiments keep the original fixture and tolerance unchanged.
The default remains elementwise RoPE; none of these variants is accepted for
deployment. `-MatrixRope` expresses each token's split-half rotation as a batched
MatMul. `-SplitRope` splits each constant into its FP16 high part and residual,
duplicates the input using HTP Concat, and accumulates both coefficient parts in
one MatMul. Only static constants are transformed on the CPU. These dense
diagnostic matrices are not a performance optimization or a claim of physical
FP32 execution.

| RoPE path | Original score failures | Candidate score failures | Matched-input score failures | Original final residual failures |
| --- | ---: | ---: | ---: | ---: |
| Elementwise baseline | 46 | 45 | 21 | 2 |
| Matrix, both blocks | 30 | 27 | 14 | 1 |
| Split matrix, both blocks | 12 | 10 | 3 | 1 |
| Split matrix, only Block 1 | 10 | 7 | 0 | 2 |

Block 0 passes in all three experiments. The both-block matrix variant also has
two original Q-norm and two Q-RoPE violations; the both-block split variant has
one of each. The isolated variant has no such extra violations. All hardware
chain tasks still exit 1, with positive accelerator profiles and checked cleanup.

The matrix variant matches the FP16-coefficients/FP32-sum simulation in
131,070 of 131,072 RoPE outputs, but matches FP32 arithmetic with final FP16
rounding in only 110,032. Splitting the coefficients raises the latter agreement
to 131,035 (both blocks) and 131,036 (isolated Block 1). This demonstrates an
HTP precision improvement without inferring an exact undocumented kernel.
For the isolated variant, ideal FP32 RoPE followed by FP16 output and FP64 QK
still gives ten original score failures: the remaining chain error is not
explained by its 36 RoPE bit mismatches alone.

`GLM-OCR matrix RoPE chain`, `GLM-OCR split RoPE chain`, and
`GLM-OCR split RoPE isolated block` preserve separate captures/reports under
`build/ocr-chain-matrix/`, `build/ocr-chain-split/`, and
`build/ocr-chain-split-local/`. The last adds `-RopeBlock1Only` to `-SplitRope`.
`GLM-OCR RoPE variant analysis` analyzes the first two;
`GLM-OCR isolated RoPE analysis` adds
`--chain-compare-build experimental/snapdragon/build/ocr-chain-capture`.
This comparison verifies baseline executable/capture/fixture hashes and asserts
bit identity of every Block-0 tap, both inputs, and Block-1 norm1/QKV/Q/K norms.
The isolated score improvement is therefore measured at unchanged upstream
tensors. All eighteen taps pass the matched-input oracle in this run, but the
original chain gate still fails. Original residual indices 34755 and 48323,
their values, operands and signed error decomposition remain exactly as above.

### Upstream rounding evidence

The analyzer reconstructs norm1, QKV projection/bias and Q/K norms in FP64 from
observed inputs and the verified native constants. In the both-block split run,
QKV matches a single final rounding in 196,541 of 196,608 values, versus only
155,565 for a separately rounded projection followed by bias. A double-rounded
bias path is therefore not the leading explanation. Q/K norms match single
rounding in 65,351 and 65,357 of 65,536 values respectively.

The original Q-norm failure at index 53629 has upstream effect -0.00324110427
and local norm effect +0.00000457392. Its four-way chain decomposition attributes
-0.00302384607 to propagated Block-0 execution. This is not evidence for a broken
local RMSNorm. Removing all Q/K norm and RoPE rounding offline leaves two score
failures, or one when also replacing QKV with an ideal projection from observed
norm1. These are diagnostic counterfactuals, not implementable HTP guarantees.

The both-block split residual failure at index 38104 is an exact sum of observed
operands `2.703125 - 2.67578125`: actual 0.02734375 versus original 0.02416849136,
allowed error 0.00312084246. Propagated Block-0 error contributes +0.00120663643
and local Block-1 error +0.00175642967; cancellation factor is 222.51. Better RoPE
does not monotonically remove every downstream threshold crossing.

Do not substitute matched-input references for the chain gate or relax tolerance.
After these experiments the default 128-patch receipt/German hardware gates and
native verifier/tokenizer/image/position regressions all pass. Full encoder, OCR
quality and performance remain unvalidated.

### Block-0 output decomposition

The existing chain analyzer now also reconstructs Block 0. Run
`GLM-OCR isolated RoPE analysis` for the baseline Block-0 computation and
`GLM-OCR RoPE variant analysis` for the both-block matrix/split captures. This is
offline development analysis only: no new hardware graph, runtime change, model
weight edit or CPU inference fallback. Reports remain in each run directory's
`chain-boundary-analysis.json`, under `block0_output_analysis`.

The pinned Block-0 oracle is regenerated in memory; input, constants and both
reference tensors must match the fixture bytes exactly. FP64 attention projection
and MLP reconstructions must independently match the candidate oracle within
`1e-5 + 1e-5 * abs(reference)`; context uses absolute `1e-6`. This reconstruction
check does not replace or change the native acceptance tolerance. Fifteen signed
terms sum back to the observed Block-0 output error, checked at every value within
1e-12 (measured closure 0 in all three captures). Nonlinear contributions use an
explicit order of counterfactual boundary replacements, not causal percentages.

For the baseline computation, all Block-0 taps still pass. Output RMSE versus
the original oracle is 0.00072464835, maximum absolute error 0.02152252197.
Selected output-component RMS magnitudes are:

| Component at Block-0 output | RMS magnitude |
| --- | ---: |
| Initial input/weight conversion | 0.000240753 |
| Attention context propagated through output projection | 0.000321796 |
| Local attention projection | 0.000186429 |
| First residual addition rounding | 0.000328524 |
| Residual-input error propagated through MLP | 0.000332304 |
| Local norm2 propagated through MLP | 0.000106429 |
| Local SiLU propagated through down projection | 0.000307734 |
| Local down projection | 0.000157541 |
| Final residual addition rounding | 0.000347876 |

These RMS values must not be added as independent error budgets. Signed terms
can cancel. Reference FP32/FP64 differences are retained explicitly in the
decomposition rather than being attributed to HTP execution.

Both residual additions match correctly rounded FP16 sums in all 65,536 values.
The local projection/norm results also mostly match single rounding: attention
projection 65,521/65,536; norm2 65,428/65,536; gate projection 262,058/262,144;
up projection 262,033/262,144; down projection 65,499/65,536. This points to
representation loss at these boundaries, not a grossly incorrect addition or
matrix layout. Keeping only the last observed sum unrounded reduces Block-0
output RMSE to 0.00062926006 in an FP32-input oracle experiment; it is not an
implemented wider-precision HTP handoff.

### Attention and SiLU localization

The attention-context contribution is further decomposed, with closure below
2e-15, through exact FP64 softmax/context and the verified output weights:

| Attention contribution at Block-0 output | RMS magnitude |
| --- | ---: |
| Upstream Q/K through softmax | 0.000056982 |
| Score dot/output rounding through softmax | 0.000081815 |
| Local softmax path | 0.000290872 |
| Value branch | 0.000036497 |
| Context MatMul | 0.000061090 |

Thus the local softmax path is the largest attention-context contribution under
this ordered decomposition. It includes output rounding and any effects of the
composed ReduceMax/Subtract/Exp/ReduceSum/Divide path, not a proven defect in a
particular QNN primitive. Both-block split gives a similar local softmax RMS
0.000294035, despite the more accurate RoPE.

SiLU matches a single final FP16 rounding in only 99,266/262,144 values (37.87%).
Simulating FP16 rounding after each operation of the native factor-first formula
matches 120,302 values (45.89%). A separately rounded reciprocal variant matches
126,732 (48.34%); neither reproduces the kernel. Both positive and negative gate
values disagree. At the Block-0 output, single SiLU output rounding contributes
RMS 0.000099872, simulated intermediate rounding 0.000199720, and the observed
minus staged-simulation remainder 0.000311358. Their signed sum closes below
6e-15. These components are correlated; the remainder does not identify whether
Exp, Divide, fusion or another implementation detail is responsible.

### Controlled Block-1 sensitivity

Three interventions pass changed Block-0 outputs to the pinned candidate-weight
Block-1 oracle: an observed-SiLU tail replay, the same replay with single-rounded
SiLU, and the exact observed final residual sum without FP16 rounding. The replay
rounds gated multiply, down projection/bias, and final addition separately. Its
control differs from the captured output in 1,151/65,536 elements, so it is not a
bit-exact HTP emulator. Reports include the control and paired SiLU effect.

Single-rounded SiLU reduces baseline Block-0 output RMSE from 0.00072464835 to
0.00068425348 (control 0.00072490308), but worsens conditional Block-1 score RMSE
from 0.00091725909 in the replay control to 0.00107889185. At the two originally
failing Block-1 residual indices, paired conditional shifts are:

| Index | Single-rounded SiLU minus replay control |
| --- | ---: |
| 34755 | -0.00070703030 |
| 48323 | +0.00132393837 |

The first reduces the existing positive error; the second increases it. For the
both-block split capture, the paired shift at its failing residual index 38104
is also adverse (+0.00078034401). An isolated improvement in average Block-0
accuracy therefore does not guarantee improvement at every downstream boundary.

Reports additionally show `frozen_local_error_estimate_vs_original`, formed by
adding each conditional oracle delta to the captured Block-1 tensor. This assumes
unchanged local Block-1 execution error and is NOT a hardware prediction, proof
of attainable precision, or acceptance gate. Even the observed-SiLU control can
move borderline failure counts; raw counts from these estimates must not be
reported as a working fix. Conditional deltas and original failing indices are
retained separately.

All three capture analyses pass their identity, reconstruction and closure checks.
That offline investigation required no new native build or hardware execution.
The subsequent intermediate-tensor hardware measurement is recorded below.

### Internal HTP tensor measurement

`GLM-OCR internal tensor capture` builds the opt-in `-CaptureInternals` mode,
which requires `-TestHtp -CaptureHtp`. It uses the split-RoPE-only-in-Block-1
configuration and separate `build/ocr-chain-internals/` output. Four existing
softmax tensors (row maximum, shifted scores, exponential, row sum) and four
SiLU tensors (decay exponential, divisor, numerator exponential, sigmoid factor)
become additional APP_READ outputs. There are no extra arithmetic nodes.
Existing tap layouts, fixtures and default graph choices remain unchanged.

Each block runs three times with NaN-sentinel buffers, finite checks, bit-exact
internal repeatability, positive accelerator profiling and checked cleanup.
The final-execution internals are written as `0.internals.f16` and
`1.internals.f16` through the existing CREATE_NEW capture path and SHA-256 report.
The instrumented executable remains ARM64/no CRT/Kernel32-only, SHA-256
`b5fd53bfc1ea33f78897d53cea183988170b3e81ecc69475903a279336a4b9cc`.
The runtime DLL identities and original chain fixture are unchanged.

Run `GLM-OCR internal tensor analysis` to produce `internal-analysis.json`.
The existing exporter uses `--analyze-internal-tensors`, the usual chain fixture
and build options, and `--chain-compare-build` pointing to
`build/ocr-chain-split-local/`. It checks both executable/capture/fixture hashes,
exact geometry and input/handoff identities, and matching runtime/RoPE settings.
It then asserts that both inputs and ALL original taps of BOTH blocks are
bit-identical to the uninstrumented control. This assertion passes: adding these
diagnostic outputs did not perturb any observed existing tensor on this run.
Both internal captures are finite and repeatable. The chain gate still exits 1
with the same ten original score/two residual violations; measurement success
does not constitute a precision fix.

#### Observed rounding rules

The analyzer compares each local operation to FP64 arithmetic on its observed
operands, including nearest-even, nearest-with-ties-away-from-zero, and
toward-zero FP16 results. The following matches hold in BOTH blocks:

| Operation | Matching rounding model | Matching values per block |
| --- | --- | ---: |
| Softmax maximum | Exact | 1,024 / 1,024 |
| Softmax subtract | Nearest, ties away from zero | 65,536 / 65,536 |
| Softmax sum | Nearest-even | 1,024 / 1,024 |
| SiLU divisor addition | Nearest, ties away from zero | 262,144 / 262,144 |
| SiLU final multiply | Nearest, ties away from zero | 262,144 / 262,144 |
| Gated multiply | Nearest, ties away from zero | 262,144 / 262,144 |
| First/final residual additions | Nearest-even | 65,536 / 65,536 each |

For example, all 59,035 Block-0 SiLU-divisor disagreements with nearest-even
occur exactly at midpoints. Its final multiply's 507 disagreements are likewise
midpoints. This resolves those apparent arithmetic discrepancies without calling
them approximation defects. The residual additions include thousands of
midpoints and select nearest-even instead, so there is no single global HTP
rounding rule established by these graphs. Row sums have no exact midpoints in
this fixture and therefore do not distinguish the two nearest rounding modes.

#### Division and exponential results

| Local operation | Block-0 nearest-even matches | Block-1 nearest-even matches |
| --- | ---: | ---: |
| Softmax Exp | 61,367 / 65,536 | 61,331 / 65,536 |
| SiLU decay Exp | 242,604 / 262,144 | 242,891 / 262,144 |
| SiLU numerator Exp | 252,525 / 262,144 | 252,459 / 262,144 |
| Softmax Divide | 38,393 / 65,536 | 37,350 / 65,536 |
| SiLU factor Divide | 137,611 / 262,144 | 138,870 / 262,144 |

All observed exponential results lie within the two FP16 numbers bracketing
their FP64 reference. Division differs more substantially: 6,440/7,214 softmax
results and 27,138/26,025 SiLU-factor results lie outside that bracket in
Blocks 0/1. Neither changing the midpoint rule nor truncating the exact quotient
reproduces them. A rounded-reciprocal/multiply model also does not match fully.
Both divisions have a negative mean error: softmax -4.5391e-6/-4.6479e-6 and
SiLU factor -1.6568e-4/-1.6332e-4. These are measured graph-path discrepancies
relative to observed operands, not a claim about undocumented kernel internals.

Telescoping decompositions close exactly in both blocks. At Block-0 softmax
output, local RMS contributions are shift 3.0113e-6, Exp 4.6021e-6, sum
6.9418e-6, and Divide 1.2410e-5. At SiLU output they are decay Exp 1.6796e-5,
numerator Exp 2.4146e-5, divisor addition 6.8420e-5, factor Divide 1.0044e-4,
and final multiply 6.1123e-5. Divide is the largest local contribution in each
of these ordered decompositions, not the sole error source or an independent
percentage of final chain error.

The default 128-patch receipt/German HTP gates were rebuilt and both pass;
verifier, tokenizer, RGB preprocessing and position regressions also pass.
The intermediate capture itself is currently measured on the 64-token chain;
128-token storage is bounded but has not been hardware-validated in this mode.
No tolerance relaxation, original-weight change or full-model acceptance follows
from this diagnostic result.

### HTP quotient residual correction

Two opt-in experiments now apply `q + (a - q*b)/b`, starting with the native
Divide result `q`. All five operations are HTP FP16 nodes; no CPU neural work or
unproven FP32 arithmetic is assumed. The shared tool-private graph helper keeps
the unmodified Divide when the experiment is disabled or the block index is 0.
The original graph tensor limit remains unchanged.

- `-RefineDivide` enables correction of Softmax and SiLU quotients in Block 1.
- Adding `-RefineSiluOnly` corrects only the SiLU quotient and requires
   `-RefineDivide`.
- Tasks `GLM-OCR quotient correction capture` and `GLM-OCR SiLU quotient capture`
   use internal captures, split RoPE only in Block 1, and separate directories
   `build/ocr-chain-quotient/` and `build/ocr-chain-quotient-silu/`.
- Corresponding `GLM-OCR quotient correction analysis` and
   `GLM-OCR SiLU quotient analysis` tasks compare to the instrumented, uncorrected
   `build/ocr-chain-internals/` control. Reports include the enabled flags.

Both hardware runs pass finite/repeatability checks for internal outputs and
positive accelerator profiling, with checked cleanup and the ARM64/Kernel32-only,
no-CRT audit. Both still exit 1 for the unchanged chain precision gate.

The combined experiment asserts bit identity of all Block-0 tensors, both block
inputs, Block-1 taps through scores, and Softmax maximum/shift/Exp/sum. The
SiLU-only experiment additionally asserts identity through norm2/gate/up and
SiLU decay/divisor/numerator. All assertions pass. SiLU operands change in the
combined experiment because corrected Softmax changes the upstream computation;
the isolated SiLU experiment supplies the same-operand comparison instead.

| Same-operand local comparison | Uncorrected | Corrected |
| --- | ---: | ---: |
| Softmax nearest-even matches / 65,536 | 37,350 | 56,935 |
| Softmax quotient RMSE | 0.000011317660 | 0.000007202706 |
| Softmax outside adjacent FP16 bracket | 7,214 | 492 |
| SiLU factor nearest-even matches / 262,144 | 138,870 | 224,104 |
| SiLU factor quotient RMSE | 0.000213555449 | 0.000130305647 |
| SiLU factor outside adjacent FP16 bracket | 26,025 | 863 |

Softmax local RMSE improves about 36%; isolated SiLU factor RMSE about 39%.
The mean quotient errors move from -4.6479e-6 to +2.8425e-7 for Softmax and
from -1.6332e-4 to +4.0194e-5 for isolated SiLU. Correction reduces the negative
bias but does not produce correctly rounded quotients everywhere.

All ten original/seven candidate score violations remain bit-for-bit unchanged:
scores precede both corrections. Each variant has one original and one candidate
final-residual violation, versus two original/one candidate in the control.
All other taps pass. The original failing residuals now pass, but a new one
appears in both variants:

| Final output index | Original oracle | Control | Either corrected variant | Allowed error |
| --- | ---: | ---: | ---: | ---: |
| 34755 | 0.1301323175 | 0.1337890625 | 0.1328125000 | 0.0036506616 |
| 48323 | 0.0546834469 | 0.0585937500 | 0.0566406250 | 0.0032734172 |
| 12136 | 0.1332283020 | 0.1308593750 | 0.1289062500 | 0.0036661415 |

At index 12136 the new error is -0.0043220520. Overall final-output RMSE versus
the original oracle is 0.0011068444 in the control, 0.0010537251 with both
corrections, and 0.0011120240 with SiLU only. Fewer threshold violations do not
by themselves prove a globally more accurate chain. Neither option is enabled
by default or accepted as a full precision fix.

The analyzer also simulates correction from the observed control quotient using
the measured ties-away rule for product/residual/add and an ideal single-rounded
correction Divide. For Softmax this predicts 65,391/65,536 corrected outputs
exactly; it is not an exact HTP emulator. Product rounding erases 36,232 nonzero
exact Softmax residuals and 117,683 isolated-SiLU residuals. Those are offline
model counts, not additional captured hardware residuals. The report labels
whether the modeled correction is active in the corresponding capture and keeps
an exact-residual counterfactual separate from measured outputs.

The evidence supports testing a more accurate residual calculation before simply
repeating this FP16 iteration. The current unchanged scores also require separate
upstream work; a post-score correction cannot fix them. No wider-precision
residual implementation or performance claim has been validated here.
Default receipt/German HTP examples, native verifier/tokenizer/image/position
tests, and the original internal-capture analysis were rerun successfully.

### Captured matrix residual: local precision verified, chain still fails

The next opt-in experiment computes the Block-1 SiLU correction residual through
MatMul accumulation instead of separately rounding `q*b`. For each group of 256
elements it multiplies `[a, q]` by vertically concatenated `[I; -diag(b)]`, giving
`a - q*b`. The diagonal construction uses only exact zero/one masks and negation
of the observed FP16 divisor. Quotient, residual, correction Divide and final
Add still have FP16 tensor interfaces. This does not claim a documented physical
accumulator width or universally correctly rounded MatMul implementation.

Reproduction:

- `GLM-OCR matrix residual capture` builds with `-MatrixResidual -RefineDivide
   -RefineSiluOnly -CaptureInternals -CaptureHtp -TestHtp -SplitRope
   -RopeBlock1Only`, using the unchanged chain fixture and
   `build/ocr-chain-matrix-residual-group256/`.
- `GLM-OCR matrix residual analysis` compares to `build/ocr-chain-internals/`.
- Block 1 appends `silu_initial_quotient` and `silu_correction_residual`, each
   `[tokens,4096]`, to its eight existing internal captures. Block 0 retains eight.
   The analyzer verifies all capture/fixture/executable/runtime identities and
   uses FP64 arithmetic on the actual captured operands.

The initial scalar-batch graph and a 32-element grouped graph were deliberately
stopped during QNN finalization after more than eight and six minutes respectively.
Their separate build directories `ocr-chain-matrix-residual` and
`ocr-chain-matrix-residual-grouped` retain aborted-run evidence, not numerical
results. Grouping 256 elements reduces the 64-token batch count to 1,024 and
allows finalization and execution to complete. This costs substantial temporary
storage: the explicit identity alone is 128 MiB at 64 tokens (256 MiB at the
128-token capacity). No performance or production suitability claim is made;
the matrix-residual path was measured only on the 64-token chain.

The completed native executable SHA-256 is
`9d3ff36d6f60a970b9487ecb990a8a54a11526179be41b44bee5d5fd44fc9b44`.
It passes the ARM64 Kernel32-only/no-CRT audit. Both blocks execute three times;
internal captures are finite and bit-repeatable, with successful cleanup.
Both inputs, all Block-0 taps/internals, Block 1 through gate/up and the first
seven internals are bit-identical to the uncorrected control. The newly captured
initial quotient is also bit-identical to the control SiLU factor.

| Block-1 SiLU measurement | Uncorrected Divide | Separate-product correction | Matrix-residual correction |
| --- | ---: | ---: | ---: |
| Nearest-even quotient matches / 262,144 | 138,870 | 224,104 | 261,969 |
| Quotient RMSE | 0.000213555449 | 0.000130305647 | 0.000111144337 |
| Quotients outside adjacent FP16 bracket | 26,025 | 863 | 0 |

All 262,144 captured matrix residuals match exactly one nearest-even FP16
rounding of `a - q*b` computed in FP64, including 4,288 midpoint cases.
Residual RMSE is 3.0338583e-8; no nonzero exact residual is erased. The earlier
separate-product offline model has residual RMSE 0.000131390785 and erases
117,683 nonzero residuals. This directly verifies the intended improvement
against identical operands, rather than inferring precision from tensor types.
The remaining 175 quotient disagreements are all within the adjacent FP16
bracket. An offline ideal correction-Divide model predicts 261,967 outputs
exactly; the correction quotient itself was not captured, so this is not an
exact account of its kernel or a guarantee of fully correct rounding.

The unchanged chain gate still fails: ten original/seven candidate score
violations, plus two final-output violations against each oracle. Other taps
pass. Scores precede the changed arithmetic and are bit-identical to control.
Final-output RMSE versus the original oracle changes from 0.001106844425 to
0.001084375442, but the individual failures remain decisive:

| Final output index | Original oracle | Control | Matrix-residual path | Status |
| --- | ---: | ---: | ---: | --- |
| 34755 | 0.1301323175 | 0.1337890625 | 0.1328125000 | Now passes |
| 48323 | 0.0546834469 | 0.0585937500 | 0.0585937500 | Still fails |
| 12136 | 0.1332283020 | 0.1308593750 | 0.1289062500 | New failure versus control |

The local residual hypothesis is confirmed, not full-chain acceptance. Improving
the correction residual alone does not remove upstream representation and
accumulation errors. Default inference arithmetic, source weights and numerical
tolerances remain unchanged. Further work should localize the remaining output
errors and upstream score errors separately, not repeat correction blindly.

## Next stages

1. **Execution contract and tokenizer: completed above.** Initial source audit and
   bounded tokenizer are verified; image and numerical contracts remain below.
2. **RGB preprocessing and positions: completed above.** Image-file decoder reuse
   and real scan fixtures remain separate integration work. Extend the numerical
   oracle to learned-layer taps before claiming a correct model forward pass.
   PDF rasterization is a separate feature, not an implicit external dependency.
3. **HTP capability and precision probes: primitives, weight audit, patch and block 0 completed above.** Continue with
   larger vision grids, accumulated block error, merger, text attention/mRoPE and KV
   updates. Test real dimensions, finite outputs and cleanup on the installed SDK.
   Weight casting ranges are audited; activation and accumulated errors remain open. No blind
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

Current status: source acquisition, verification, bounded native tokenizer/prompt
runtime, RGB resize/normalization/patch packing, single-image positions, isolated
FP16 HTP primitives, complete weight range audit, learned patch projection and
complete vision block 0 on three 64-patch grids and two visible 128-patch OCR
examples against two numerical oracles, with PNG previews and known input text.
No image-to-text inference, full-model FP16 accuracy, PDF support or OCR quality
acceptance is claimed. No previous Whisper or TranslateGemma campaign is restarted.