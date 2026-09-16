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

## Next stages

1. **Execution contract and tokenizer: completed above.** Initial source audit and
   bounded tokenizer are verified; image and numerical contracts remain below.
2. **RGB preprocessing and positions: completed above.** Image-file decoder reuse
   and real scan fixtures remain separate integration work. Extend the numerical
   oracle to learned-layer taps before claiming a correct model forward pass.
   PDF rasterization is a separate feature, not an implicit external dependency.
3. **HTP capability and precision probes: primitives, weight audit and patch tap completed above.** Continue with
   vision attention/axial RoPE, merger, text attention/mRoPE, norms, SiLU and KV
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
FP16 HTP primitives, complete weight range audit and a learned patch-projection tap.
No image-to-text inference, full-model FP16 accuracy, PDF support or OCR quality
acceptance is claimed. No previous Whisper or TranslateGemma campaign is restarted.