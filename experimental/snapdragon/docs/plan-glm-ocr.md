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
complete vision block 0 on three small grids against two numerical oracles.
No image-to-text inference, full-model FP16 accuracy, PDF support or OCR quality
acceptance is claimed. No previous Whisper or TranslateGemma campaign is restarted.