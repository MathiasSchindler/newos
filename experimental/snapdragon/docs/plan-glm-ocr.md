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

### Remaining chain error localization

The `GLM-OCR remaining chain error analysis` task applies the pinned conditional
oracle to the completed matrix-residual capture, comparing with the instrumented
uncorrected control. It writes `chain-boundary-analysis.json` in
`build/ocr-chain-matrix-residual-group256/`. No native graph, source weight or
tolerance changes are part of this investigation. The exporter additionally
decomposes the Block-1 MLP at each failing output and checks its FP64 reconstruction
against the matched-input oracle. Both the boundary and local decompositions
close with zero measured error. These are ordered, signed effects, not independent
percentages or runtime predictions.

All eighteen Block-1 taps pass when compared with an oracle supplied exactly the
captured Block-1 input. This does not mean zero local error: inherited and local
errors are individually below the gate but combine beyond it. The handoff is
bit-exact, ruling out a changed transfer buffer in this run.

For the ten score failures, Block-0 propagated and Block-1 local effects have the
same sign at every failing index. The Block-0 term is larger in six cases; the
Block-1 term in four. Threshold excess ranges from 0.37% to 39.33%, so these are
not all infinitesimal boundary disagreements. At the worst normalized score,
index 45338, the error is -0.004783705 versus an allowed 0.003433464:

| Signed contribution at score 45338 | Error |
| --- | ---: |
| Initial input/weight cast | -0.000089675 |
| Ideal Block-0 handoff rounding | -0.000187568 |
| Block-0 execution propagated | -0.002794795 |
| Block-1 local execution | -0.001711667 |

Within that local term, the path through QKV contributes -0.000961029, Q/K
normalization -0.000625768, RoPE -0.000097456 and the final QK dot/output rounding
only -0.000027413. Across the ten failures, mean absolute dot/output contribution
is 0.000012979. Removing just the last dot rounding is therefore not a plausible
general fix. Offline unrounded normalization/RoPE/dot from observed QKV leaves
three original score failures; also reconstructing QKV from observed norm1 leaves
one. These counterfactuals localize accumulated representation loss, not an
implemented wider-precision HTP path.

The two final-output failures are 17.89% and 19.46% over their respective limits:

| Signed output contribution | Index 12136 (token 11, channel 872) | Index 48323 (token 47, channel 195) |
| --- | ---: | ---: |
| Initial input/weight cast | -0.000392675 | +0.000416994 |
| Ideal handoff rounding | +0.000536919 | -0.000226736 |
| Block-0 execution propagated | -0.001715183 | +0.001671314 |
| Block-1 local execution | -0.002751112 | +0.002048731 |
| Total error | -0.004322052 | +0.003910303 |
| Allowed magnitude | 0.003666142 | 0.003273417 |

At index 12136 the native final sum is exactly `2.5 - 2.37109375 = 0.12890625`.
At index 48323 it is exactly `2.7421875 - 2.68359375 = 0.05859375`.
The last addition introduces no error at either point. The oracle-based
cancellation factors are approximately 36.6 and 99.2: small operand errors matter
much more relative to their small difference.

The new local MLP decomposition identifies the larger Block-1 terms as the
residual operand directly (-0.001157045/+0.000538826), that operand propagated
through the MLP (-0.000350292/+0.000804588), and local down-projection output
(-0.000973434/+0.000614248). SiLU contributes only
-0.000127025/+0.000156480 after the matrix-residual correction.
The first residual additions and both down-projection outputs match correct
nearest-even FP16 rounding from their observed operands. In particular, FP64
down projections -2.3701203157233977 and -2.684207998134184 correctly round to
-2.37109375 and -2.68359375. The first lies only about 0.000003128 from the
midpoint between neighboring FP16 values, making the result sensitive to small
upstream changes. This is evidence of finite representation precision, not a
wrong rounding decision at that projection.

The next useful precision target is therefore the residual/projection values
before cancellation and the accumulated Block-0/QKV/norm path, not another
SiLU quotient iteration or a wider final addition alone. A higher-precision
representation or later rounding would need a measured native implementation;
declaring tensors FP32 is not proof. Only two of the 24 vision blocks have been
chained here, so the low failure count does not establish full-encoder accuracy.
The existing isolated-RoPE control analysis also passes the extended analyzer.

### Late down-projection and residual rounding

An offline all-output probe first computes the Block-1 down projection in FP64
from observed gated values and fixture FP16 weights, adds the observed residual
operand, then rounds only the final sum to FP16. This predicts one remaining
original/candidate output violation instead of two; it does not predict a fully
passing chain. The hardware experiment implements this bounded proposal without
changing earlier arithmetic.

`build-ocr.ps1 -FuseDownResidual` enables `OCR_FUSE_DOWN_RESIDUAL` and requires
internal captures. It affects only the Block-1 final output. The graph concatenates
`[gated, residual, 1, zeros]` and multiplies by `[down_weight; I; down_bias; zeros]`.
Its shapes are `[tokens,5152]` and `[5152,1024]`, including 31 zero padding rows
after the bias row. All tensor interfaces remain FP16; no CPU neural arithmetic
or silent fallback is introduced. The original down-projection tap remains an
independent diagnostic output, so the experiment deliberately duplicates work
and adds approximately 10 MiB of constant storage. It is not a latency optimization.

Tasks `GLM-OCR late residual capture` and `GLM-OCR late residual analysis`
use `build/ocr-chain-late-residual/` and compare with
`build/ocr-chain-matrix-residual-group256/`. They retain the previous split RoPE
and matrix-residual SiLU settings. The native executable SHA-256 is
`ee7768967c7472f2814345362f98217263f4294fe5d591fbebb2c38040afa1ce`;
its ARM64 Kernel32-only/no-CRT audit passes. The 64-token hardware graph finalizes,
executes three times, records positive accelerator profiles and cleans up.
Internal captures are finite and bit-repeatable.

The analyzer verifies fixture/executable/capture hashes and identical runtime and
control settings. Both inputs, every Block-0 tap, all Block-1 taps through the
diagnostic down projection, and all internal captures in both blocks are
bit-identical to control. Only Block 1's final output changes: 15,944 values.
There are no new original/candidate threshold violations. Original/candidate
score counts remain ten/seven because their computation precedes the changed path.

| Output measurement | Previous matrix-residual path | Late-rounding HTP path |
| --- | ---: | ---: |
| Original output violations | 2 | 1 |
| Candidate output violations | 2 | 1 |
| Original output RMSE | 0.001084375442 | 0.001087702344 |
| Candidate output RMSE | 0.001055530955 | 0.001058229537 |

The HTP final output matches the FP64-then-FP16 reference in 65,500/65,536 values,
versus 49,588 matches for the previous separate-rounding output. Maximum deviation
from that rounded reference is 0.00048828125. This verifies that the changed graph
substantially preserves precision through the sum; it does not establish exact
single rounding everywhere or a documented physical accumulator width.

| Index | Original oracle | Previous HTP | Late-rounding HTP and rounded FP64 reference | Allowed error |
| --- | ---: | ---: | ---: | ---: |
| 12136 | 0.1332283020 | 0.1289062500 | 0.1298828125 | 0.0036661415 |
| 48323 | 0.0546834469 | 0.0585937500 | 0.0579833984 | 0.0032734172 |

Index 12136 now passes with error -0.0033454895. Index 48323 improves but remains
outside tolerance: error +0.0032999516 exceeds the limit by 0.0000265343,
approximately 0.81%. It already fails in the ideal late-rounding reference, so
another implementation of that same final sum alone is not sufficient. Earlier
residual/activation errors and the unchanged scores still require upstream work.
The slightly higher global RMSE also prevents interpreting fewer violations as
uniformly improved accuracy.

The native full-chain gate deliberately still returns failure. The original
standard graph and tolerances remain unchanged; no general deployment or complete
encoder acceptance is claimed. Only this 64-token late-rounding chain was measured.
Default 128-token receipt/German HTP examples, the complete native verifier,
tokenizer, RGB/position tests and the existing control boundary analysis pass.
For the fused path, analyzer fields comparing the final output to the old rounded
down/residual taps are explicitly marked as counterfactual differences, not the
error of a separate final Add operator.

### Late attention residual: more accurate locally, rejected by chain gate

The next isolated experiment applies the same augmented-MatMul construction to
Block 1's attention projection plus incoming residual, before norm2 and the MLP.
`-FuseAttentionResidual` requires `-FuseDownResidual`; both remain opt-in.
The tool-private `attention_project_residual` helper now serves both sums, with
separate constant buffers. The attention matrix is `[2080,1024]`, concatenating
the 1024-row projection, 1024-row identity, bias and 31 zero padding rows.
The original attention projection remains a diagnostic tap. This adds roughly
4 MiB of constants and duplicates projection work; it is not a speed claim.

Tasks `GLM-OCR attention residual capture` and `GLM-OCR attention residual analysis`
write `build/ocr-chain-attention-residual/`, comparing to the preserved
`build/ocr-chain-late-residual/` control. The executable SHA-256 is
`ea16c0c75e886a4fe54b8e9dadf6a11613074e4a499e8f0bacf431b72375f5de`.
The 64-token HTP graph finalizes and executes three times with finite,
bit-repeatable internal captures, positive accelerator profiling and successful
cleanup. The ARM64 Kernel32-only/no-CRT audit passes.

Hash-checked control comparisons verify both inputs, all Block-0 taps/internals,
Block-1 taps through attention projection and all four Softmax internals as
bit-identical. Changes begin at the first residual sum, as intended. SiLU/MLP
internals now legitimately change, so they are not claimed to be bit-identical
or suitable for a same-operand quotient comparison.

The first residual output matches FP64 projection-plus-input followed by a single
nearest-even FP16 rounding in 65,512/65,536 values, versus 51,822 for the control.
There are 13,714 changed residual values. Its RMSE versus the original oracle
improves from 0.000943392861 to 0.000899957082, with no residual-tap violations.
Final-output RMSE also improves, from 0.001087702344 to 0.001039129859
(candidate: 0.001058229537 to 0.001009296587). Nevertheless, the required
no-new-failures condition is violated:

| Final output index | Original | Previous late-down path | Added late-attention path | Allowed error |
| --- | ---: | ---: | ---: | ---: |
| 35303 | -0.0164399147 | -0.0137863159 | -0.0129470825 | 0.0030821996 |
| 48323 | 0.0546834469 | 0.0579833984 | 0.0583801270 | 0.0032734172 |

Index 35303 newly fails with error +0.0034928322; index 48323 worsens to
+0.0036966801. Both are also failures of the ideal late-down reference computed
from this run's observed operands, not merely the last MatMul's rounding mismatch.
Final-output violations rise from one to two against each oracle. Ten original
and seven candidate score violations remain bit-identical; all other taps pass.
All eighteen taps still pass the matched-actual-input oracle, which remains
diagnostic rather than original-chain acceptance.

This variant is therefore rejected as a chain fix and retained only as an explicit
diagnostic option. The previous late-down-only capture remains intact. Local
single-rounding accuracy and lower global RMSE do not guarantee fewer errors after
nonlinear propagation and cancellation. Further work should address inherited
Block-0 error and residual representation across stages instead of treating these
individual failing indices as tuning targets. No tolerance, source weight or
default graph changes were made. The prior late-down analysis, standard 128-token
receipt/German HTP examples and native verifier/tokenizer/image/position tests
pass with the updated source. No full encoder or OCR-quality acceptance is claimed.

### Block-0 late down rounding: output passes, score gate regresses

The next bounded experiment starts from the late-down-only Block-1 control, not
the rejected late-attention variant. It changes only Block 0's final down
projection plus residual sum to the same augmented MatMul. The existing
`attention_project_residual` helper is reused with an independent Block-0 weight
buffer, so resident graphs do not share mutable weight contents.
`-FuseDownResidualBlock0` requires `-FuseDownResidual`; Block-1 attention residual,
RoPE and quotient settings are unchanged. Defaults and tolerances remain unchanged.

The offline `late_down_residual_block0` intervention was run first. It reconstructs
the down projection from observed gated values in FP64, adds the observed residual
and rounds once to FP16, then supplies that input to the pinned Block-1 oracle.
The model that freezes the old local Block-1 error suggests four score and two
output failures. These estimates are explicitly not hardware predictions.

Tasks `GLM-OCR Block0 late residual capture` and
`GLM-OCR Block0 late residual analysis` use
`build/ocr-chain-block0-late-residual/` and the preserved
`build/ocr-chain-late-residual/` control. The native executable SHA-256 is
`7089ef4373551b7ab2852af66f90aa02a4d202970595fcf666e52a4a3cd7d9f7`.
The 64-token graph finalizes, executes three times with finite and bit-repeatable
internal tensors, positive accelerator profiles and successful cleanup. The
ARM64 Kernel32-only/no-CRT audit passes. This adds another approximately 10 MiB
of diagnostic constants and retains the separate down tap; it is not a production
memory or performance optimization.

Hash-checked isolation proves that the initial input, Block-0 taps through down
and every Block-0 internal capture are bit-identical to control. The new Block-0
output is handed to Block 1 bit-for-bit. Block-1 code/configuration is unchanged,
but its inputs, subsequent taps and internals legitimately change. They are not
claimed to be bit-identical or same-operand comparisons.

Block 0 matches the single-rounded FP64 output in 65,490/65,536 values, versus
49,559 in the control; 15,979 handoff values change. Every Block-0 tap still passes
both fixture oracles. The actual two-block results are:

| Block-1 measurement | Late-down-only control | Added Block-0 late down |
| --- | ---: | ---: |
| Original score violations | 10 | 14 |
| Candidate score violations | 7 | 12 |
| Matched-input score violations | 0 | 7 |
| Original output violations | 1 | 0 |
| Candidate output violations | 1 | 0 |
| Original score RMSE | 0.001542708841 | 0.001523261996 |
| Original output RMSE | 0.001087702344 | 0.001025551316 |
| Candidate output RMSE | 0.001058229537 | 0.000996266459 |

All non-score Block-1 taps pass. Nine old original score failures are fixed, but
13 new ones appear, leaving 14 total; therefore the no-new-failures criterion is
not met. Both output oracles pass with no new output failures. The original
failure at index 48323 changes from 0.0579833984375 to 0.057952880859375, versus
0.05468344688415527 in the oracle. Its error is 0.0032694339752197266, only
0.0000039832592010498 below the unchanged limit. This is a measured narrow pass,
not evidence of a robust full-model margin. The control's candidate-only output
failure at 40896 is also fixed.

The analyzer now compares paired conditional oracles for the old and new captured
inputs. It separates the measured HTP change into an input-induced oracle change
and a change in local execution error, with zero closure error. Freezing the old
local error would produce four score failures; the real graph produces 14.
The change in local score error has RMSE 0.001328968531. This directly falsifies
using a frozen local error model as an acceptance prediction for changed inputs.

At new score index 28845, the conditional input effect is only +0.000046641,
but local execution error changes by +0.004714102. The current local score
decomposition attributes +0.003694293 to Q/K normalization, +0.001068136 to the
path through QKV, +0.000194086 to RoPE and +0.000055997 to the last dot/output
rounding. At 28853, local error changes by -0.004439449; current QKV and Q/K norm
effects are -0.001937043 and -0.002063787 respectively. These are ordered,
correlated counterfactual terms; they do not prove an undocumented kernel defect
or distinguish norm output rounding from its arithmetic approximation by themselves.

This experiment is not accepted as a complete chain fix despite passing both
final-output oracles. It narrows the next investigation to input-dependent
QKV/QK-normalization precision in Block 1, including the seven matched-input
score failures, rather than more blind Block-0 variants. The standard graph and
all earlier captures remain intact. The extended analyzer, prior late-down control
analysis, default receipt/German HTP examples and native verifier/tokenizer/RGB/
position regressions pass. Only this 64-token two-block experiment was measured;
no full encoder, OCR-quality or latency acceptance is claimed.

## Native image-file input: BMP24 to patch tensors

The independent image-input path now accepts actual BMP files, decodes packed
RGB, applies the existing reference-checked smart resize and normalization, and
returns the model's patch tensor. It does not execute any neural layer or QNN
graph. No numerical diagnostic mode, tolerance or checkpoint was changed.

```powershell
.\experimental\snapdragon\tools\build-ocr.ps1 -PrepareImages -Test -BuildDir experimental/snapdragon/build/ocr-image-files
.\experimental\snapdragon\build\ocr-image-files\ocr-image.exe --prepare-image input.bmp patches.f32
```

The VS Code task `GLM-OCR image file build` runs that standalone build and test.
`-PrepareImages` alone builds without requiring reference artifacts; `-Test`
also consumes the existing `models/glm-ocr-images-v2/image-fixtures.got`.
The executable remains ARM64, Kernel32-only, no CRT, no exception or CLR tables.
Neither Python nor OS image codecs are required at runtime.

Supported input is deliberately narrow: 24-bit BI_RGB, 40-byte
BITMAPINFOHEADER, positive width, bottom-up or negative-height top-down rows,
and four-byte row alignment. The decoder checks header/payload sizes, planes,
compression, reserved fields, dimensions and destination capacity before copying
pixels. File size must match the actual input, with the pixel array ending at EOF;
`biSizeImage` may be zero or the exact padded pixel size. Palette fields must be
zero. Larger DIB headers, indexed/alpha/compressed BMPs and trailing payloads are
rejected, not interpreted heuristically. BMP BGR channels become RGB before the
unchanged image processor. No implicit orientation or color-profile transform is
performed. PNG/JPEG/TIFF/WebP and PDF are not accepted by this entry point.

Bounds are 64 MiB encoded input, 10,000 pixels per source dimension, 16 million
source pixels and 256 MiB total requested image-buffer payload (encoded input,
decoded RGB, resize workspace/output and float32 patches). This is an allocation
budget, not a measured process peak. Existing geometry, aspect-ratio and patch
limits also apply; not every image inside the individual limits is accepted.
Temporary image buffers and the input handle are released on success/failure.

`ocr_image_load` in `ocr_image_file.c` returns `OcrPreparedImage`: source geometry,
resized geometry/grid/image-token count, float count and owned patch storage.
Pass an empty result object; release a previous successful result before reuse.
`ocr_image_release` frees its storage and clears the object. On load failure the
result is empty. `ocr_image_bmp` itself takes caller-owned nonoverlapping buffers;
a null RGB destination performs validated geometry inspection without decoding.

The CLI creates a NEW output file, never overwrites an existing path, and emits
one JSON record to stdout with source/target/grid dimensions, image-token count,
patch count, `features:1176` and `float32_values`. The file is raw little-endian
float32 `[grid_height * grid_width, 1176]`, in the existing merge-aware patch order
and duplicated temporal-frame layout, not a hashed fixture or an OCR result.
Retain the JSON dimensions with the tensor. Exit 0 means preprocessing and output
succeeded, 1 means input/I/O/preprocessing failure, and 2 means invalid arguments.
An output I/O failure may leave a partial file: discard outputs from nonzero exits.

Validation on Windows ARM64:

- 828 native BMP decode/rejection checks: both row directions, all four padding
  cases, channel order, guard bytes, capacity, truncated inputs and invalid fields.
- 16 actual BMP-file-to-patch comparisons against existing processor fixtures,
  covering both row directions with exact float32-byte SHA-256 equality and exact
  JSON geometry; Unicode paths, no-overwrite and invalid input/output tests pass.
- The standalone binary passes those 16 comparisons and 46 verifier regressions.
- Full image tests still pass 1,015 geometry cases, 21,652,512 resized RGB bytes,
  43,305,024 normalized values (maximum error zero), padded-row/negative tests,
  positions and 180,712 tokenizer cases plus their negative tests.

These file tests use existing reference RGB fixtures wrapped as BMP, not a new
held-out scan corpus. They establish preprocessing equivalence, not OCR quality.

## Complete native vision execution

The image-file path now feeds the learned HTP patch projection and all 24 vision
blocks, followed by post-RMSNorm, the learned 2x2 downsampling convolution and the
complete patch merger (projection, LayerNorm, GELU and gated SiLU MLP). The final
features have text width 1536. This follows the pinned `GlmOcrVisionModel.forward`,
including its connector, rather than stopping at the last transformer block.
There is still no text prefill, decode or recognized text output.

The initial supported resized grids are exactly `[1,8,8]` and `[1,8,16]`, or
112x112 and 224x112 RGB pixels. Other grids are explicitly rejected before loading
the HTP library. Input remains the bounded BMP24 path above; this is not support
for arbitrary full-resolution pages. The two outputs are `[16,1536]` and
`[32,1536]` respectively.

### Build and run

Offline development preparation uses the existing exporter and original weights:

```powershell
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --export-vision-encoder
.\experimental\snapdragon\tools\build-ocr.ps1 -TestVision -Test -BuildDir experimental/snapdragon/build/ocr-vision
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --analyze-vision-encoder
```

Equivalent VS Code tasks are `GLM-OCR full vision export`, `GLM-OCR full vision
hardware` and `GLM-OCR full vision analysis`. Export uses pinned package/source
identities and the actual Transformers vision model for both original-value FP32
and FP16-weight/input-expanded-FP32 references. Intermediate oracle activations
are not rounded to FP16. Existing differing exports are refused. The original
checkpoint is never modified.

For a native-only build use `-Vision` instead of `-TestVision`. No Python is needed
to build or execute with existing weight artifacts. The no-CRT ARM64 executable
imports only Kernel32; QNN is loaded explicitly, with no CPU neural fallback.

```powershell
$htp = (Resolve-Path experimental/snapdragon/build/QnnHtp.dll).Path
New-Item -ItemType Directory -Path experimental/snapdragon/build/my-vision-capture
.\experimental\snapdragon\build\ocr-vision\ocr-vision.exe --vision $htp experimental/snapdragon/models/glm-ocr-vision-v1 input.bmp experimental/snapdragon/build/my-vision-capture
```

Create a fresh capture directory first. Each output uses CREATE_NEW; an occupied
output path or failed write fails the run without overwriting prior data. Failed
runs may leave partial captures; never consume them as completed features.
`--check-vision WEIGHTS_DIR` validates assets without loading QNN.

### Weights and execution contract

The 26 model artifacts total 868,342,944 bytes, separate from references/images:
24 `block-NN.got` files, `shared.got` and `tail.got`. Envelope kinds 10/11/12 retain
the pinned image/config identity and SHA-256 integrity check. Each includes the
original checkpoint digest. Block records additionally carry their block index;
lengths, finite FP16 constants and bounded FP32 RoPE coefficients are checked.
Shared weights contain the patch matrix/bias and both fixed-grid RoPE tables.
Block storage is grid-independent: the selected RoPE table is inserted into the
existing block builder's constant layout. The downsampling kernel is packed to
match the processor's merge-aware patch ordering before its MatMul equivalent.

All assets are verified before QNN loads. Block/tail files are reverified on use
and must retain their preflight hashes, preventing unnoticed changes between
validation and execution. These checks establish integrity/provenance consistency,
not authentication against a malicious artifact producer.

The prototype reuses one activation buffer and one QNN context at a time. It
creates/finalizes/executes the patch graph, each block graph, and the tail graph
sequentially, freeing each previous context before reusing weight storage.
Contexts are not yet compiled/cached for low-latency repeated requests. The
diagnostic implementation retains all 18 block outputs and writes them to disk;
it is not a performance-optimized serving path. Backend/device/profile/context
cleanup and positive accelerator profile evidence are required for success.

The existing block graph now accepts a runtime mode without reference fixtures:
one execution per block, finite captured activations, probability bounds/row sums
and bit-preserving handoff. Existing probe mode still executes each block three
times and applies its original numerical gates. Optional quotient, residual and
RoPE experiment flags are not accepted by this runtime build. No tolerance or
existing default graph arithmetic was changed.

### Outputs and measured results

The capture directory contains `0.patches.f32`, `0.patch.f16`, each block's
`N.input.f16` and `N.taps.f16` (N=0..23), `24.postnorm.f16`,
`25.downsample.f16` and final `26.features.f16`. The final file is raw
little-endian FP16 `[image_tokens,1536]` in image-token order, not text or a
self-describing container. Grid dimensions are logged by the runtime. The test
runner additionally records case/grid provenance through its case identity,
executable, image, weight, four QNN runtime-library and capture hashes in the two
`*-vision-run.json` files. The analysis verifies artifact/executable/input/capture
identity, exact CPU preprocessing and every unchanged block handoff, then compares
all 28 exposed stage outputs. Runtime hashes are recorded for reproducibility.

Both complete hardware runs (pattern 64 patches, receipt 128 patches) finish with
finite captured outputs, positive HTP profiling and successful resource cleanup.
Fourteen negative tests cover missing/corrupt/truncated artifacts, wrong input
geometry, missing image/runtime and invalid arguments. The full existing image,
tokenizer, verifier and default receipt/German block-0 HTP regressions also pass.

**Numerical acceptance remains false at the unchanged 0.003 + 0.005*abs(reference)
gate.** Measured final-feature violations are:

| Case | Values | Original FP32 | Candidate FP32 |
| --- | ---: | ---: | ---: |
| Pattern | 24,576 | 2 | 1 |
| Receipt | 49,152 | 10 | 12 |

Accumulated intermediate differences are substantially larger: block 23 has
31,114/30,829 violations (original/candidate) for pattern and 61,843/62,663 for
receipt. Postnorm and merger change the scale and error distribution; the small
final violation count is NOT an OCR error rate or evidence of harmless drift.
Even the full receipt patch projection has five original-reference violations,
while its candidate-reference gate passes; the prior 16-row projection probe did
not cover this complete input.

`vision-analysis.json` contains all per-stage counts, RMSE, maxima and hashes.
The analysis command's successful exit means its structural/integrity checks and
report generation succeeded, not that `numerical_gate_pass` is true. Likewise,
native exit 0 means complete vision execution and I/O success, not numerical or
OCR-quality acceptance. This implementation enables further pipeline development
without concealing the unresolved precision work.

## Multimodal decoder input and text prefill

The native pipeline now connects freshly computed Vision features directly to
the complete 16-layer text decoder prefill and its final RMSNorm, in the same
process and QNN backend/device session. It does not reload its own feature dump
as inference input. This section records the prefill-only stage; the subsequent
[native generation stage](#native-autoregressive-generation) adds the output head,
resident cache, incremental decode and text output without changing this command.

### Official prompt contract

The [GLM-OCR model card](https://huggingface.co/zai-org/GLM-OCR), consulted on
2026-09-16, specifies the document-parsing prompts `Text Recognition:`,
`Formula Recognition:` and `Table Recognition:`. These match the existing native
prompt builder. The new input references invoke the checkpoint's actual pinned
chat template through `AutoTokenizer.apply_chat_template`, with one user message
containing the image followed by the task text and `add_generation_prompt=True`.
The single image placeholder expands to exactly 16 or 32 feature tokens.
No extra system instruction or hand-written alternate chat wrapper is inserted.

The native commands use the template default: `enable_thinking` is not passed.
The assembly helper also supports the existing explicit no-thinking variant;
both variants of all three tasks on both image grids are byte-exactly tested
(12 cases), but the complete hardware campaign uses Text Recognition with the
default template only. Arbitrary extraction/JSON-schema prompts mentioned on the
model card are not implemented by these fixed-task commands.

### Native input and decoder

`ocr_text_prepare` in `ocr_text.{h,c}` builds a bounded `OcrTextInput` with IDs,
three mRoPE position planes, modality bytes, FP16 embeddings and a causal mask.
Ordinary rows are copied from the complete 59,392x1536 embedding table. Only
`<|image|>` placeholder rows are replaced, in order, by unchanged Vision features.
The image feature count and grid geometry must agree exactly. The caller supplies
valid nonoverlapping buffers, including `image_tokens*1536` feature words and the
complete embedding table; buffers must remain valid during the call. On failure,
the result count is zero and partial array contents must be discarded.

The runtime uses a single 64-position context bucket. Text Recognition has 28/44
valid prompt tokens on the two image grids; all remaining rows are zero padded.
Padding IDs are 59246, padding modality/position entries are zero. Causality is
based on sequence indices, not the compressed image mRoPE positions. Keys beyond
the real prompt and future keys receive FP16 -65504; every masked probability
is checked to be exactly zero on hardware, with valid probability ranges/row sums.
Padded query rows are computed but excluded from accuracy scoring and are not
valid positions for subsequent token selection. This is not a tested 131072-token
runtime or a general chat/multi-image input API.

The HTP prefill implements the model's four RMSNorms per layer (input,
post-self-attention, post-attention/pre-MLP, post-MLP), Q/K/V/output projections,
16 query heads and 8 KV heads of width 128, grouped KV repetition, causal stable
Softmax, fused gate/up projection and stable gated SiLU MLP. Text RoPE uses
interleaved channel-pair rotation and mRoPE sections [16,24,24], unlike the vision
encoder's split-half rotation. Captured coefficients are byte-exact with the
pinned text rotary implementation. Each layer's output is copied bit-exactly to
the next layer. There is one QNN context at a time, freed before reusing its
constant storage, followed by a separate final norm graph.

CPU work is file handling, image preprocessing, prompt/index lookup, positions,
masks, coefficient lookup, copies and diagnostics. Neural projections, attention,
rotations, normalizations and MLPs execute on HTP; no CPU neural fallback is added.
The executable is still ARM64, Kernel32-only, no CRT/exception/CLR tables.

### Build, artifacts and captures

Use the existing toolchain entry points and previously completed Vision captures:

```powershell
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --export-text-decoder
.\experimental\snapdragon\tools\build-ocr.ps1 -Prefill -Test -BuildDir experimental/snapdragon/build/ocr-prefill
.\experimental\snapdragon\tools\build-ocr.ps1 -TestPrefill -Test -BuildDir experimental/snapdragon/build/ocr-prefill
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --analyze-text-prefill
```

VS Code tasks: `GLM-OCR text decoder export`, `GLM-OCR multimodal input tests`,
`GLM-OCR multimodal prefill hardware`, `GLM-OCR multimodal prefill analysis`.
`-Prefill -Test` runs the exact input/negative tests without loading QNN; `-Prefill`
alone builds without requiring artifacts. Native execution needs no Python.

```powershell
$htp = (Resolve-Path experimental/snapdragon/build/QnnHtp.dll).Path
New-Item -ItemType Directory -Path experimental/snapdragon/build/my-prefill
.\experimental\snapdragon\build\ocr-prefill\ocr-prefill.exe --prefill-text $htp experimental/snapdragon/models/glm-ocr-vision-v1 experimental/snapdragon/models/glm-ocr-text-v1 input.bmp experimental/snapdragon/build/my-prefill
```

`--prefill-formula` and `--prefill-table` select the other fixed tasks. A fresh,
existing capture directory is required; files are CREATE_NEW and failed runs may
leave incomplete diagnostics. `--test-text-input TEXT_DIR` is the native offline
input-reference check. These prefill commands do not generate text.

The 18 model artifacts under `models/glm-ocr-text-v1/` are `embeddings.got`
(kind 13, 182,452,384 bytes), 16 `text-NN.got` files (kind 14, 61,354,148 bytes
each), and `text-shared.got` (kind 15, 68,768 bytes), totaling 1,164,187,520 bytes.
Shared data contains final-norm gamma and a 64-position FP32 rotary lookup table.
Kind 16 holds the 12 input test records and is not needed for native inference.
Original checkpoint/config identities, exact sizes, layer indices, finite FP16
weights, bounded rotary constants and SHA-256 envelopes are verified. All weights
are checked before QNN loads; streamed layer assets must retain their preflight
hashes. The unused MTP layer 16 and LM output head are not exported here. Original
weights remain untouched, and previous Vision-only/test paths remain available.

In addition to the existing Vision capture files, the runtime writes:

- `0.text-layout.u32`: count, image-token count, task, no-think flag, grid height,
   grid width, signed mRoPE delta stored as its 32-bit representation.
- `0.text-ids.u32`, `0.text-positions.i32`, `0.text-modalities.u8`,
   `0.text-mask.f16`, `0.text-embeddings.f16`, `0.text-cosine.f32`, `0.text-sine.f32`.
- `N.text-input.f16` and `N.text-taps.f16` for N=0..15. Taps are four normalized
   states and final layer output (each [64,1536]), rotated K [64,8,128], V
   [64,1024], and attention probabilities [16,64,64], totaling 688,128 FP16 values.
- `16.text-norm.f16`: final [64,1536] states; the last valid prompt row is
   `count-1`. These are hidden states, not logits or text.

In the original prefill-only stage, K/V outputs were captured diagnostics. The
generation extension now retains their valid rows in a resident RAM cache. All
graphs are still finalized per run and all taps are retained, so no serving
latency/memory optimization is claimed. Run reports include image, executable,
Vision/text artifact, QNN runtime and capture hashes.

### Validation and remaining precision

Both integrated hardware runs (pattern/receipt) complete all Vision and text
layers with finite captured outputs, accelerator profile evidence and clean
resource teardown. Native and offline checks establish exact official-template
IDs, embedding/feature insertion, position/modality/mask arrays, rotary values,
and all 16 unchanged text handoffs. The test suite adds 12 synthetic assembly
cases/10 rejection checks, 12 exact pinned-model input fixtures, and 11 artifact/
argument negative checks. The prior full image/tokenizer/verifier and default
receipt/German HTP block regressions pass; standalone image export also passes.

The two FP32 text oracles are **conditioned on the actual captured native Vision
features**, isolating the text integration from the known Vision error. They are
not original-checkpoint end-to-end image-to-text references. Both use the actual
16-layer text model with no KV cache, original or FP16-expanded weights, explicit
positions and padding masks. Only real prompt rows are scored at the unchanged
`0.003 + 0.005*abs(reference)` gate.

Layers 0..13 pass both references for both cases. Pattern layer 14 has one
candidate-only violation; layer 15 has three violations in each scope/case.
After final norm:

| Case | Valid values | Original violations | Candidate violations | Last-prompt-row original/candidate |
| --- | ---: | ---: | ---: | ---: |
| Pattern | 43,008 | 1,326 | 1,330 | 39 / 41 |
| Receipt | 67,584 | 1,276 | 1,268 | 20 / 20 |

A local FP64 RMSNorm reference on the captured layer-15 input and deployed gamma
has zero tolerance violations in both cases (RMSE about 0.000358/0.000359). This
distinguishes propagated input differences from the norm's local implementation
error; it is not a claim of mathematically exact normalization or physical FP32
accumulation. `prefill-analysis.json` records counts, last-row counts, RMSE,
maxima, identity checks and this local comparison. Numerical acceptance remains
false; successful native/analysis exits mean execution/structural checks and
report generation, not accuracy or OCR-quality approval. No tolerances were
relaxed and no previous diagnostic precision variant became a default.

## Native autoregressive generation

This section records the initial 64-position generation milestone. The
[256-position extension](#extended-context-and-retained-graphs) below supersedes
its current limits and adds an optional reusable-graph path.

The bounded native BMP-to-text path is now connected end to end: Vision,
multimodal prefill, the untied 1536-to-59392 LM head, deterministic greedy
selection, incremental single-token decoding through all 16 layers, and UTF-8
output. No Python, CRT or CPU neural fallback is present in the executable.
The original checkpoint and earlier artifact packages remain unchanged.

### Commands and limits

```powershell
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --export-generation
.\experimental\snapdragon\tools\build-ocr.ps1 -Generate -Test -TestTokenizer -BuildDir experimental/snapdragon/build/ocr-generate
$htp = (Resolve-Path experimental/snapdragon/build/QnnHtp.dll).Path
New-Item -ItemType Directory -Path experimental/snapdragon/build/my-generation
.\experimental\snapdragon\build\ocr-generate\ocr-generate.exe --generate-text $htp experimental/snapdragon/models/glm-ocr-vision-v1 experimental/snapdragon/models/glm-ocr-text-v1 experimental/snapdragon/models/glm-ocr-generation-v1 input.bmp experimental/snapdragon/build/my-generation 32
```

Use `--generate-formula` or `--generate-table` for the other fixed prompts.
`MAX_NEW_TOKENS` must be a decimal integer from 1 to 64. The total prompt plus
generated-token budget is also capped at 64: default Text Recognition therefore
allows at most 36 generated tokens for the 8x8 grid and 20 for 8x16. Larger pages,
long documents, arbitrary prompts and the config's 131072-position capacity are
not supported. Each invocation processes one image, starts with a fresh valid
cache prefix, and requires an existing fresh capture directory (CREATE_NEW).
Failed runs may leave partial output/captures. No resumable or multi-request API
is promised.

Stdout contains only incrementally decoded UTF-8 bytes; native diagnostics use
stderr. Broken output writes fail the run. Byte-pair pieces spanning UTF-8
characters are buffered until complete; terminal incomplete/invalid byte sequences
use the tokenizer's replacement behavior. Special tokens are skipped for display
but remain in model history. EOS IDs 59246 and 59253 and `do_sample=false` are
verified against the checkpoint generation configuration. Selection scans the
complete finite vocabulary, keeps the lowest ID on ties and never silently masks
unused IDs. A selected ID >=59282 has no native tokenizer piece and fails loudly.

Exit codes: 0 means EOS reached, 3 means incomplete output stopped by the token
or context limit, 1 means runtime/artifact/output failure, 2 means invalid CLI
syntax/limit. EOS takes precedence over a simultaneous limit. Neither exit 0 nor
3 implies numerical or OCR-quality acceptance. Forced process termination is
possible, but graceful cancellation and persistent serving are not implemented.

### Execution and ownership

`ocr_prefill.c` now parameterizes the existing text layer for 64 prefill queries
or one decode query. Prefill retains only valid rotated K and V rows, excluding
padding, into two static arrays [16,64,8,128] of FP16 values: 4 MiB total. Each
decode graph concatenates the past prefix and the newly computed K/V on HTP;
only after successful execution are new rows copied into the resident cache.
Each layer checks its expected cache length before use. Position for a generated
input is `past_sequence_length + mrope_delta`, identical on all three axes.
Decode attends the exact past+current length without future/padded keys.

The cache is **RAM-resident, not device-resident**: prefix tensors are copied as
graph constants. One QNN context exists at a time and is freed before replacing
constant storage. Graphs are re-finalized and layer/head artifacts re-read and
checked against their preflight hashes on every step. This is a correctness-first
implementation, not an efficient serving or latency claim. Embeddings and shared
rotary/norm data remain in memory. Every neural projection, normalization, rotary
operation, attention and MLP runs on HTP; CPU work includes lookup/copies,
validation, cache ownership, finite greedy comparison and UTF-8 detokenization.

The LM head is eight independent HTP MatMul chunks of 7424 vocabulary columns,
whose complete FP16 logits are joined before selection. `models/glm-ocr-generation-v1/`
contains eight kind-17 `head-NN.got` files (22,806,692 bytes each, source identity,
chunk index and SHA-256 envelope), the native `tokenizer.got`, and a manifest.
The first head input is the last valid prompt row, never the padded row 63;
subsequent head inputs are the single decoded token's final normalized state.

### Reproduction and evidence

VS Code tasks `GLM-OCR generation export`, `GLM-OCR generation native tests`,
`GLM-OCR generation hardware` and `GLM-OCR generation analysis` reproduce the
three-token campaign. The `GLM-OCR bounded generation hardware` and corresponding
`GLM-OCR bounded generation analysis` tasks run to EOS or the context boundary.
Equivalent build flags are `-TestGenerate -MaxNewTokens 3` or `64`; analysis uses
`export-glm-ocr.py --analyze-generation --generation-build BUILD_DIR`.

Native validation passes 180712 tokenizer fixtures, six incremental UTF-8 split/
truncation/malformed cases, finite argmax/tie tests, both EOS IDs and limit
precedence, plus 15 generation artifact/CLI negative cases. Existing verifier,
multimodal input and image/runtime-negative checks remain in the build runner.

Both three-token image runs complete with HTP profile evidence, finite outputs
and clean resource teardown. Pattern IDs [2721,6237,5557] produce the prefix
`\x60\x60\x60markdown`; receipt IDs [21656,2035,71] produce `BELEG`. All six selected
tokens agree with both original and FP16-expanded text/head references, conditioned
on native Vision features and the actual emitted token prefixes. Reference caches
evolve independently, rather than substituting native K/V into the oracle.

The longer bounded campaign also passes the structural/reference checks. Pattern
emits an empty Markdown code block and reaches EOS 59253 after six tokens (exit 0).
Receipt emits `BELEG 1042\n16.09.2026\n` in 20 tokens and reaches the 64-position
prompt-plus-output budget (exit 3, explicitly incomplete). All 26 decisions agree
with both reference scopes, including EOS. The campaign validates 24 incremental
decode steps across the two cases, with all 16 layer cache prefixes checked per
step. Original/candidate logit violations remain in every step; these runs establish
execution and control flow, not numerical approval or complete-document quality.

The analyzer verifies capture/weight/executable hashes, bit-exact resident K/V
prefixes against the earlier taps, embedding/layer/head handoffs, decode positions,
greedy full-vocabulary selection, UTF-8 output, and stop reasons. Per-step hidden
states, logits and local head-on-native-input errors use the unchanged
`0.003 + 0.005*abs(reference)` gate. The three-token original/candidate logit
violation counts are 990/986, 2090/2079, 6966/6910 for pattern and 643/643,
1740/1746, 421/407 for receipt. **Numerical acceptance remains false**, despite
matching greedy decisions. These conditional references are not an original-model
end-to-end image oracle or a held-out OCR quality assessment.

Generation adds `STEP.head-input.f16`, `STEP.logits.f16`,
`LAYER.decode-PP-input.f16`, `LAYER.decode-PP-taps.f16`,
`LAYER.decode-PP-cache.u8` (SHA-256 of actual K and V prefixes),
`PP.decode-layout.u32` (past length, position, input token, prompt count), and
`PP.decode-norm.f16`. Decode taps hold four norms and layer output (5x1536),
new rotated K and V (2x1024), then [16,past+1] attention probabilities.
`0.generated-ids.u32` includes EOS if reached; `0.generation-result.u32` contains
count, reason (1 EOS, 2 output limit, 3 context limit), prompt count and requested
limit. `0.generated.txt` exactly matches the native UTF-8 stream. Reports retain
the numerical rejection and do not turn execution success into accuracy success.

## Extended context and retained graphs

The next implemented milestone separates the unchanged 64-row image/text prefill
from a **256-position decode cache**. The default generation package is now
`models/glm-ocr-generation-v2/`; v1 and its captures are retained as historical
evidence. The original model weights and text/vision packages are unchanged.

`--generate-text`, `--generate-formula` and `--generate-table` now accept
`MAX_NEW_TOKENS` from 1 to 256. Prompt plus generated-token budget is capped at
256, leaving 228/212 output tokens for the two default Text Recognition prompts.
The supported images remain 8x8/8x16 patch-grid BMP24 inputs. This does not expand
the prefill bucket or establish support for larger pages.

The v2 package adds kind-18 `decode-rope.got`: 262304 bytes containing checkpoint
identity and FP32 cosine/sine tables for all 256 positions. It is bounded,
identity/hash/finite-range checked before QNN loads. The original 64-position
prefill table remains separate. KV capacity is now 16 MiB; generated-token and
UTF-8 storage are separately sized, and capture names handle three-digit indices.
Native tests cover 64/99/100/255 capture suffixes, 22 negative artifact/limit cases,
three accepted extended CLI limits and the existing tokenizer/input regressions.
Hardware reaches past the former position-64 boundary; full capacity through 255
is bounded in code, not claimed as an exhaustive hardware sweep.

### Optional graph reuse

Build with `-Generate -ReuseDecode` to retain 16 decode-layer and eight LM-head
graphs and their independent weight storage. This remains **opt-in**: the default
variable-length path is the numerical/performance comparison, not silently
replaced by a different attention layout.

Decode graphs use six dynamic inputs: the current hidden row, cosine, sine, past
K, past V and the causal/padding mask. The past-input tensors have 255 rows; the
newly computed current K/V is concatenated in slot 255. Only the valid past prefix
and slot 255 may receive attention. All other probabilities must be exactly zero.
Current K/V is appended into the real chronological cache slot only after the
layer succeeds. Cache-prefix hashes and coefficient captures verify the actual
inputs used across repeated graph executions. This keeps shape constant while
past length changes, without freezing cache or RoPE values as graph constants.

The 24 retained contexts are freed before their independently allocated weight
buffers. Failure cleanup follows the same ownership order; ready flags are reset,
and a new generation clears cache storage. This is still a single-request CLI,
not a tested persistent service. The final norm graph is still rebuilt per token;
Vision and prefill also remain cold-built. Neural computation stays on HTP and
the executable remains ARM64/Kernel32-only/no-CRT. Approximate retained raw weight
storage is 1.16 GB in addition to backend allocations and existing buffers; this
is a speed/memory tradeoff, not a measured peak-memory claim. KV buffers are in
process RAM and copied through QNN inputs, not zero-copy device-resident storage.

### Independent reference and timing

`export-glm-ocr.py --reference-generation --generation-build BUILD_DIR` loads the
full pinned original model with BF16 weights expanded to FP32, preprocesses the
original BMP independently, and generates its own token sequence and cache. It
does not consume native Vision features or force native token prefixes. The report
`independent-generation.json` records source/image/model/executable hashes, text,
EOS, top-two logit margins, and exact character edit distances against both the
native output and the existing receipt transcription. It can create reference
results before a native run finishes, marking unavailable comparisons as null.

Both native modes now finish the receipt with EOS after 53 tokens:

```text
BELEG 1042
16.09.2026
2 Hefte: 7,90 EUR
1 Stift: 2,05 EUR
Summe: 9,95 EUR
```

Pattern reaches EOS after six tokens, emitting an empty Markdown code block.
Retained-mode outputs are byte-identical to the independent original model for
both cases. Receipt differs from the literal stored transcription only by its
missing terminal newline: one edit out of 74 characters, CER 0.0135135 for both
native and reference. No whitespace normalization hides this discrepancy. This
tiny development fixture set is not a held-out or multilingual quality corpus.

The conditional layer/cache analysis also passes for both modes: all 59 token
decisions agree with original and FP16-expanded text/head references. Actual
retained-cache prefixes, masks, handoffs and decode rotary coefficients pass.
However, later logits differ between variable-length and fixed-layout execution;
the retained pattern run has up to 14350 original-reference logit violations in
one step, versus 8571 in the variable-length run. **The numerical gate remains
false in both modes.** No tolerance was relaxed and graph reuse is not approved
as numerically equivalent merely because these texts match.

`0.timing.u64` stores QPC frequency followed by ticks for file reads, graph
finalization subset, graph execution subset, generation context operations,
Vision, prefill, head, decode and total wall time. Phase totals overlap with
operation totals and must not be summed. The two graph subsets cover the shared
Vision-tail/text/head execution helper, not all older Vision-block operations.
Analysis converts these to seconds and retains the raw hashed capture.

Observed receipt timings: variable-length mode 582.02 s total, 474.13 s decode,
67.04 s head; retained mode 62.43 s total, 16.52 s decode, 2.28 s head. The retained
run was serial, but part of the earlier variable-length campaign overlapped other
development work. These values locate a major graph-build cost; they are **not a
controlled speedup benchmark**. Even retained execution still spends about 28.69 s
in Vision and 10.89 s in prefill. The separate CPU reference generated the receipt
in about 9.93 s excluding model load, so NPU execution alone is not a performance
win. Future comparisons need isolated repeated runs and peak-memory measurement.

### Reproduction

```powershell
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --export-generation
.\experimental\snapdragon\tools\build-ocr.ps1 -TestGenerate -ReuseDecode -MaxNewTokens 212 -BuildDir experimental/snapdragon/build/ocr-retained-full
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --analyze-generation --generation-build experimental/snapdragon/build/ocr-retained-full
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --reference-generation --generation-build experimental/snapdragon/build/ocr-retained-full
```

Tasks: `GLM-OCR context256 hardware/analysis`, `GLM-OCR retained full
hardware/analysis`, `GLM-OCR retained independent reference`. The native binary
and reference reports remain in their separate build directories. Historical v1
analyses need their matching v1 package explicitly; do not mix old reports with
new binaries or weights.

## PNG and larger image grids

Native file input now recognizes **BMP24 and static PNG by signature**, including for
`--prepare-image`, `--vision`, `--prefill-*` and `--generate-*`. PNG decoding uses
the existing in-tree zlib inflater through a tool-private Windows allocation
bridge. It adds no CRT, image DLL, Python or neural CPU fallback to production.

PNG supports grayscale at 1/2/4/8/16 bits, palette at 1/2/4/8 bits, RGB,
grayscale-alpha and RGBA at 8/16 bits, all five scanline filters, Adam7 and
consecutive split IDAT chunks. Palette alpha and gray/RGB tRNS keys are supported.
Alpha is composited on white in integer sample space; 16-bit samples are rounded
to RGB8 using their full range, not truncated. Embedded ICC profile chunks are
accepted but ignored after basic name/compression-method checks. There is no
color management or gamma conversion. EXIF-bearing PNG and APNG remain rejected
rather than silently discarding orientation/animation semantics. Other ancillary
chunks have no pixel effect. CRCs, zlib/Adler integrity, exact decoded length,
stream consumption, chunk names/order and IEND/EOF are checked. Duplicate PLTE,
unknown critical chunks and decompression excesses are rejected.

Input remains capped at 64 MiB, 10000 per source dimension and 16 Mi pixels.
PNG compressed-plus-decoded workspace is capped at 128 MiB and conservatively
included in the 256 MiB preprocessing buffer budget. This is not the total QNN
process memory limit. Failed writes can leave partial captures; they are not
successful results, and an existing output file is never overwritten.

The ordinary build preserves its two original buckets and 64-row prefill.
Build with `-LargeImages` plus `-Vision`, `-Prefill` or `-Generate` to enable:

| Patch Grid (H x W) | Resized Pixels (W x H) | Image Tokens |
| --- | --- | --- |
| 8 x 8 | 112 x 112 | 16 |
| 8 x 16 | 224 x 112 | 32 |
| 16 x 16 | 224 x 224 | 64 |
| 16 x 32 | 448 x 224 | 128 |

The 2026-09-17 import update keeps the existing model-compatible resize whenever
it already selects a supported grid. Otherwise GUI and engine share a fitted
shape policy: square canvas, or a 2:1 canvas when source width/height exceeds
1.5; the entire source is antialiased-bicubic resized proportionally and centered
on white. Canvas height is 224 in the large build, 112 in the small build.
There is no cropping, rotation or tiling. The GUI previews the original image;
the source's resolution is not the inference resolution. Small text in page-sized
inputs may be lost by downsampling; accepting an image does not establish useful
full-page OCR quality. Resource and extreme-aspect-ratio limits still apply.

`--prepare-image` preserves the original model preprocessing contract.
`--prepare-image-fit INPUT OUTPUT.f32` exports the large-build fitted patches
and their geometry without QNN; `--vision`, `--prefill-*` and `--generate-*` use
the fitted policy automatically. The large build
uses 256-row prefill for all four grids. Total prompt plus generated output is
still 256 tokens: the large default Text Recognition prompts contain 76/140
tokens, leaving at most 180/116 output tokens, respectively. Longer documents
can therefore still end with exit 3 (incomplete limit).

The large build defaults to a separate `models/glm-ocr-vision-v2/` directory.
All 26 original Vision weight artifacts retain their layout. New kind-19
`large-rope.got` has 393376 bytes: original checkpoint identity followed by FP32
cosine/sine tables for 16x16, then 16x32, in pinned model patch order. The native
reader checks exact length, envelope/source identity, integrity and finite
coefficient range before graph construction. The text v1 and generation v2
packages remain unchanged; the existing 64-position prefill coefficient table
covers the compressed spatial positions of these grids. Decode uses its
separate 256-position coefficient table.

Vision buffers now cover 512 patches in the large build; prefilling 128 image
tokens requires the expanded text buffers/masks and valid-prefix KV copies.
Neural computation still executes through HTP, with per-stage finite values,
accelerator profiles, chronological cache hashes and checked cleanup. Optional
`-ReuseDecode` is independent of `-LargeImages` and remains opt-in. Full tensor
captures can consume gigabytes; this is still a diagnostic runtime.

### Validation and results

`GLM-OCR image file build` followed by `GLM-OCR PNG import tests` passes 486
native PNG/fit cases: exact BMP/PNG patch equality across all legal color/depth
combinations and filters, Adam7 including tiny/empty-pass geometries, palette
and tRNS alpha, split IDAT and compression levels, plus CRC/Adler/truncation,
chunk-order/name, dimension, unsupported-format and decompression-length errors.
The <=8-bit generated PNG pixels are independently cross-checked with Pillow;
16-bit compositing is checked against integer reference pixels. Four fitted
portrait/landscape/square images are byte-exact against Torchvision's uint8
antialiased bicubic resize plus white padding, the same resize backend as the
model. Pillow's separate resize implementation rounds differently and is not
used as the resize oracle. Six additional native fit cases check canvas geometry,
padding, capacities and extreme legal aspect ratios. The GUI import tests cover
a transparent 400x600 palette portrait and a 896x896 screenshot.
The installed GUI/engine update completed two real screenshot-to-text requests
with the exact expected four-line transcription, plus cancel, close-active and
missing-engine tests. Desktop and compact screenshots were inspected. Reports,
screenshots and previous executables are preserved under
`data/ocr-png-import-717000672efc4aafaba60cd164f539c9/`; the new temporary raw
tensor captures were removed after validation. Existing application runs were
not deleted. Restart an older open OCR window to use the new importer.
The existing image, resize, tokenizer and prompt fixtures still pass, including
43305024 exact normalized patch values. Both small Vision-only and large
generation builds pass the ARM64/Kernel32-only/no-CRT audit. Large-RoPE tests
reject missing/truncated/corrupt and correctly rehashed nonfinite data.

The new 224x224 text sample and 448x224 receipt are rendered directly at their
new resolutions, not enlarged from the old bitmaps. Their PNG, RGB, expected
transcription, font and reference hashes are recorded in the v2 manifest.
Both PNGs complete the full 24-block Vision, connector, 16-layer prefill and
autoregressive HTP path to EOS: 29 tokens for the text sample and 53 for receipt.
Both texts match an independently executed original full model byte for byte.
The sample reads `OCR 1042`, `19,95 EUR`, `16.09.2026`, `Hello!`; the receipt
matches the five-line transcription shown in the preceding milestone.
Both omit the stored transcription's final newline, counted as one edit rather
than normalized away. These are development fixtures, not held-out page tests.

Native image patches are exact against the official processor. Prompt IDs,
embeddings with captured image-feature insertion, modalities, positions, masks
and prefill RoPE are exact against the actual model. All 82 greedy decisions
also match both conditional original/candidate references; cache prefixes and
decode handoffs pass. **The numerical gate remains false**, unchanged at
`0.003 + 0.005 * abs(reference)`. Initial large-grid final-feature violations
are 3208/3168 (original/candidate) for the sample and 2253/2582 for receipt,
with much larger intermediate drift and nonzero logit violations. Matching
texts do not establish harmless numerical error or general OCR quality.

The initial retained-graph runs took approximately 95.64 s and 125.85 s total,
including about 40.73/41.06 s in prefill. These single runs identify substantial
graph-building cost; they are not a controlled throughput benchmark or a claim
that NPU execution beats the CPU reference. Reports record subsequent runs'
own hashed timings and preserve the numerical rejection.

### Reproduction

From the repository root, use the named VS Code process tasks or:

```powershell
.\experimental\snapdragon\build\gemma-oracle-x64\python.exe -B experimental/snapdragon/tools/export-glm-ocr.py --export-vision-encoder --large-images --vision-output experimental/snapdragon/models/glm-ocr-vision-v2
.\experimental\snapdragon\tools\build-ocr.ps1 -TestGenerate -LargeImages -ReuseDecode -MaxNewTokens 212 -BuildDir experimental/snapdragon/build/ocr-large-png
```

Then run `GLM-OCR large PNG vision analysis`, `GLM-OCR large PNG generation
analysis` and `GLM-OCR large PNG independent reference`. Their reports are
`vision-analysis.json`, `generation-analysis.json` and
`independent-generation.json` under `build/ocr-large-png/`. PNG development tests
use the existing optional exporter tool, not a production Python path.

For your own supported PNG/BMP, create an empty capture directory and run:

```powershell
$runtime = (Resolve-Path experimental/snapdragon/build/QnnHtp.dll).Path
.\experimental\snapdragon\build\ocr-large-png\ocr-generate.exe --generate-text $runtime experimental/snapdragon/models/glm-ocr-vision-v2 experimental/snapdragon/models/glm-ocr-text-v1 experimental/snapdragon/models/glm-ocr-generation-v2 INPUT.png NEW_CAPTURE_DIRECTORY 116
```

Exit 0 means EOS, 3 means incomplete output at an explicit limit, and 1 means
failure. Stdout is recognized UTF-8 text; diagnostics go to stderr. Original
checkpoint files and earlier v1 packages/captures are preserved.

## Native OCR GUI

`src/tools/ocr/ocr_gui.c` is a native Win32 front end in the same visual style
as the TranslateGemma experiment. It has a PNG/BMP file chooser, editable Unicode
path, aspect-preserving image preview, Text/Formula/Table selector, Recognize,
Cancel, Copy, read-only scrolling output and status. Controls scale with per-monitor
DPI and remain bounded at the minimum 460x620 logical-pixel window size.

The GUI reuses the native image decoders and fitted shape policy before launching
inference; common dimensions no longer fail with "Unsupported grid". It starts
the adjacent `ocr-generate.exe --serve` once and retains that child between
requests, with large images and reusable decode/head graphs. Vision/prefill
contexts reload from the bounded cache described below. The UI remains
responsive and reads stable UTF-8 output through its timer. Native diagnostics
and partial captures stay under `build/ocr-app/runs/<tick>-<pid>/`, including
`execution.log` and `stdout.txt`. Paths are quoted for CreateProcess directly;
no shell or Python participates in production execution.

Cancel and closing during recognition terminate only the child process owned
by that GUI instance. This is process cancellation, **not graceful QNN context
teardown**; partial text/captures are explicitly incomplete. Normal completion
frees the request's transient context; retained graphs remain until server EOF
or process termination. CLI completion and normal server EOF use checked teardown.
Closing the GUI also terminates an idle owned server. The GUI exposes EOS completion, context-limit
incompletion and engine errors as distinct statuses. Existing model/runtime
integrity checks remain in the child engine.

Build from the repository root:

```powershell
.\experimental\snapdragon\tools\build-ocr.ps1 -Generate -LargeImages -ReuseDecode -GraphCache -AppMode -Test -BuildDir experimental/snapdragon/build/ocr-app
.\experimental\snapdragon\tools\build-ocr.ps1 -Gui -BuildDir experimental/snapdragon/build/ocr-app
```

Use `GLM-OCR GUI integration` for native window tests and `GLM-OCR GUI layout
check` for screenshots without inference. These extend the existing optional
`test-translate-gui.py` helpers via `--ocr` and `--ocr-layout`; the default
TranslateGemma test behavior remains unchanged. The GUI itself is ARM64/no-CRT,
importing only Kernel32, User32, Gdi32 and the Windows Common Dialog API. The
engine still imports Kernel32 only, with explicitly loaded QNN libraries.

Verified: Unicode paths, real PNG preview, repeated complete Text Recognition,
invalid-image rejection, missing engine, cancellation/recovery, closing during
an active request, and desktop/compact layouts with all controls in bounds.
The two GUI text requests matched the stored sample (excluding its terminal
newline) and took 95.20/96.90 s, with first output at 81.28/82.83 s. These are
integration observations, not controlled benchmarks. Formula/Table selectors
route to the existing engine commands; their OCR quality is not established by
the Text Recognition GUI fixtures. The Open file dialog and clipboard action
use native Windows controls; their full interactive behavior is not exhaustively
automated in the current tests.

## Full Vision numerical cause analysis

`GLM-OCR numerical cause analysis` runs `export-glm-ocr.py --diagnose-vision`
against the preserved `build/ocr-large-png/` captures and Vision v2 artifacts.
The report is `vision-cause-analysis.json`. It checks the pinned original model,
reference source, native executable, captured tensors and deployed block weights.
All 24 blocks on both larger images are compared with the actual reference block
fed the **observed native input**, separating inherited error from new local
execution error. The signed decomposition closes within 1e-10. Its components
are correlated differences, not independent percentages of error.

The additional ideal-input consistency check found a reference construction
problem: historical full-Vision `candidate.half().float()` also rounded nonpersistent
RoPE frequency buffers. The deployed rotary tables use original FP32 frequencies.
The new diagnostic rounds parameters only, leaves buffers unchanged, and checks
that blocks 13, 15 and 23 reproduce the corrected full-model stages on ideal
inputs. Original FP32 references and historical artifacts were not overwritten.
Historical candidate figures must therefore not be described as comparisons
against precisely the deployed constants. At block 23, this reference mismatch
alone has RMSE 0.12309 (pattern) / 0.39482 (receipt). It does **not** explain away
the failures against the unchanged original model.

With the corrected candidate, the strongest selected amplification occurs in
the MLP branch (block numbers are zero-based):

| Case / block | Incoming error RMS | Attention-branch change RMS | MLP-branch change RMS | Inherited output RMS | New local output RMS |
| --- | ---: | ---: | ---: | ---: | ---: |
| Pattern / 13 | 0.007856 | 0.003535 | 0.081707 | 0.084749 | 0.001337 |
| Pattern / 15 | 0.120267 | 0.009764 | 0.365011 | 0.433996 | 0.008223 |
| Pattern / 23 | 0.488805 | 0.101309 | 3.867442 | 3.888558 | 0.059252 |
| Receipt / 13 | 0.006138 | 0.003301 | 0.046725 | 0.048751 | 0.001356 |
| Receipt / 15 | 0.056728 | 0.003721 | 0.164719 | 0.182077 | 0.006587 |
| Receipt / 23 | 0.247014 | 0.055128 | 2.440612 | 2.449528 | 0.053487 |

These are finite-input differences between reference executions, not derivatives
or predictions for an untested precision fix. FP32 addition remainders are below
4.5e-6 RMS in these decompositions. Block 23 still has 142,250 / 251,941 global
output failures for pattern / receipt; local matched-input failures are 16,554 /
32,424. A local pass is not a full-chain pass.

FP64 checks on captured operands further distinguish implementation from input
error. Both residual additions in blocks 0, 13, 15 and 23 match nearest-even FP16
rounding at every element in both images. Softmax and SiLU are not generally
single-round FP64 results, although these selected local primitive checks stay
within the existing tolerance. Norm and rotary checks are also reported; there
are isolated rotary violations. This is evidence of accumulated representation
and approximation error amplified by the model, not a faulty copy or a proven
last-addition bug. It does not identify every undocumented HTP kernel behavior.

The text model's final RMSNorm has zero local tolerance violations on the actual
layer-15 output in both images (393,216 values each, including padded rows), with
RMSE 0.00036850 / 0.00036150. Its global mismatch therefore cannot be assigned to
a bad final norm alone. The investigation does not yet decompose every text
layer, connector or generation logit error.

No native arithmetic, original weights or tolerance was changed for this
investigation. Numerical acceptance remains **false** at
`abs(error) <= 0.003 + 0.005 * abs(reference)`, with finiteness required. Next
precision work should test better intermediate/residual precision before the
sensitive MLP branches, then rerun full-chain gates; another isolated last-add
or activation adjustment is not justified as a complete fix.

## OCR profiling and benchmark

`GLM-OCR profiling build` creates a separate large-image, retained-decode build
under `build/ocr-benchmark/`. `GLM-OCR serial benchmark` runs fresh native processes
serially, using the same two images and model/runtime identities as the preserved
large-PNG baseline. `GLM-OCR benchmark supplement` appends one run of each image
and preserves the initial report as `benchmark-initial.json`. Results and all
individual run/capture hashes are in `benchmark-results.json`.

The six measured runs all completed at EOS and matched **every baseline
nontiming capture bit-for-bit**, including intermediate tensors, logits, cache
hashes, token IDs and text. The added instrumentation did not change captured
arithmetic. Three receipt runs met the original 300-second observation budget:

| Measurement | Pattern (one in-budget run) | Receipt (three in-budget runs) |
| --- | ---: | ---: |
| Total elapsed seconds | 92.56 | median 122.78; range 120.36-128.62 |
| First output seconds | 78.51 | median 104.52; range 102.43-110.25 |
| Generated tokens, including EOS | 29 | 53 |
| Process peak working set | 5083.78 MiB | median 5135.29 MiB |
| Tensor capture time | 1.31 s | median 2.62 s |

Two initial runs had extreme wall times: pattern 14,387.36 s and receipt
8,720.94 s, with most of each gap recorded inside prefill finalization. Their
process CPU times were only 138.30 / 136.02 s. The reason for the wall-time gaps
is **not established**; suspension or an external stall cannot be distinguished
from these counters. Windows process waits did not enforce elapsed-wall timeout
across those gaps. They remain in the report, explicitly excluded from the
in-budget summary rather than silently deleted. This small development benchmark
is not a production latency guarantee or a quality-controlled CPU/NPU comparison.

For the receipt run at the elapsed-time median (122.78 s):

| Phase | Calls | Graph finalization seconds | Graph execution seconds |
| --- | ---: | ---: | ---: |
| Vision blocks | 24 | 49.29 | 3.23 |
| Text prefill | 16 | 36.20 | 0.95 |
| Incremental decode | 832 | 6.64 | 4.51 |
| LM head | 424 | 1.02 | 0.66 |
| Decode final norm | 52 | 0.81 | 0.04 |

Vision plus prefill finalization alone costs 85.49 s, about 70% of elapsed time.
All graph executions together take about 9.39 s. Capture I/O writes about
1.26 GiB and takes 2.66 s in this run; disabling captures alone cannot remove
the startup bottleneck. Execution includes host-side QNN overhead, not just DSP
kernel time. Detailed QNN profiling and full APP_READ tensors are enabled;
filesystem caches were not flushed. Peak working set is for the child process,
not total system memory or separate DSP allocations. GUI polling/painting is
excluded from this CLI benchmark.

New capture files `0.profile-header.u64` and `0.profile-graphs.u64` use version 1.
The header contains version, QPC frequency, row count, capture ticks and bytes.
Each seven-u64 row contains phase, layer/chunk, build ticks, finalize ticks,
execute ticks, accelerator cycles and accelerator microseconds. Phases 1-8 are
patch, Vision block, connector, prefill, prefill norm, decode, decode norm, head.
Retained decode/head executions have zero finalize ticks. Serialized Vision/prefill
cache hits include loading and integrity validation in the preparation/finalize
column, not actual graph finalization. Build timing starts at first tensor
registration and excludes earlier graph/context preparation. The existing
`0.timing.u64` counters overlap phases and must not be added to these totals.
This is graph-level profiling with accelerator counters, not a per-operator
kernel report. Row capacity and expected phase counts are checked.

Original prioritized follow-up (the first item and capture-file suppression are
now implemented below; lean graph outputs and the other items remain):
1. Retain or serialize/reload Vision and prefill contexts across requests; measure
   reload time and memory with explicit model/shape/runtime identity checks.
   A resident GUI engine is useful only if it actually retains these graphs.
2. Specialize prefill buckets to real prompt lengths (76 / 140 rather than 256
   padded rows), with new exact mask/KV and full numerical checks.
3. Add a lean mode without diagnostic tensors, then measure changed fusion,
   memory/copy costs and output equivalence. Retain the small decode norm graph.
4. After startup costs fall, examine decode execution and device-resident KV
   updates with a genuine operator-level profile.

## Resident engine and bounded graph cache

The September 17 app update adds `-GraphCache` and `-AppMode` to the existing
freestanding build. Neither changes weights, operators, tensor precision or the
numerical gate. The diagnostic build without `-AppMode` still writes all taps;
app mode skips `.f16`/`.f32` capture files, while preserving text, IDs, small state
records, profiles and finite/mask checks. Diagnostic APP_READ graph outputs and
their host transfers remain, so this is not yet a lean-output graph optimization.

`ocr_graph_cache.c` stores at most 40 fixed files beside the executable under
`graph-cache/`: 24 Vision blocks and 16 prefill layers. Each slot holds only the
last matching shape/constants, rather than accumulating entries for every image.
Each file is capped at 128 MiB (5 GiB total upper bound); the tested 16x16 raster
uses 1,837,819,136 bytes (1.71 GiB). Switching incompatible rasters replaces slots.
The key hashes the executable, all four deployed QNN runtime files, tensor
descriptors and every STATIC tensor's bytes, including RoPE/mask constants.
The versioned header, saved tensor IDs and binary payload are SHA-256 checked.
Tensor IDs are rebound after restore because QNN assigns different IDs within a
resident process. Missing, stale or damaged files trigger normal HTP compilation;
a rejected QNN restore fails the request. There is no CPU neural fallback.
Readers cannot open an entry while it is being written. Partial files after
termination are detected on the next request. Graph construction and cache
validation still occur before restore; only finalization is replaced.

The server fixes model/runtime directories at launch. Each pipe request contains
four little-endian u32 values (image UTF-16 code-unit count, capture-directory
count, task 0..2, token limit 1..256), followed by both non-NUL UTF-16 paths.
Image paths are bounded below 32768 units, capture paths below 32700. It writes
streamed `stdout.txt`, `execution.log` and a closed `done.u32` status (0 EOS,
3 incomplete, 1 failure) inside the supplied new capture directory. It retains
the backend/device, profile, 16 decoder and eight head contexts; KV contents and
counts reset for every request. Failure exits the server. GUI cancellation and
close terminate only its owned child, and the next request starts cleanly.
The resident RAM/HTP working set remains allocated while the GUI is open.

Verification used the user's `build/ocr-app/gui-compact.png`, serially on the
same Snapdragon machine with unchanged model/runtime files. Single observations:

| Full screenshot request | Native total | Preparation/finalization | Graph execution |
| --- | ---: | ---: | ---: |
| Previous fresh-process diagnostic engine | 135.207 s | 109.579 s | 7.994 s |
| New app, cache creation and first resident request | 174.871 s | 146.273 s | 8.301 s |
| New app, second request in same process | 37.588 s | 9.772 s | 8.401 s |

The warm sample is 3.60x faster end to end, not a measurement of NPU utilization
or a general throughput guarantee. Preparation timings varied noticeably across
runs. Native times exclude GUI startup and do not provide operator-level HMX
occupancy. Cache creation is expensive; deployment can carry forward the cache
only with the exact tested executable/runtime. No sleeps or parallel inference
were introduced for these comparisons.

All generated IDs through EOS, UTF-8 and 1,062 small captures match the previous
engine exactly. An earlier three-token diagnostic comparison checked all 208
capture files, including every FP16/FP32 tensor, across two resident requests;
the second loaded all 40 cached layers with zero decoder/head re-finalization.
The full app run also verifies zero decoder/head re-finalization and no large
tensor capture files. Seventeen EOF/truncation/length/task/limit/NUL protocol
cases pass. Mutating a tensor ID, payload byte and truncating another entry
rebuilds exactly three entries, keeps 37 hits and preserves output tokens.
Native verifier/prefill/generation gates and ARM64/no-CRT PE audits pass.

The real GUI test recognizes its 896x896 PNG in 33.150 s (first text 20.522 s)
with a warm disk cache, and 26.509 s (first text 21.941 s) on repetition. Both
texts match the expected four lines; Unicode input, preview/layout, invalid image,
cancel/restart, close-active and missing-engine tests pass. This is a different
image and must not be compared directly with the screenshot timings above.
Full numerical acceptance remains false; identical outputs are regression
evidence, not proof that the original-model tolerance violations were fixed.

Reproduce using `export-glm-ocr.py --test-graph-cache --test-resident` with a
diagnostic cache build, or add `--test-app` with an app build for full EOS output
and cache-corruption checks. Supply `--vision-output`, `--generation-build` and
`--baseline-build` explicitly; the baseline must be the prior uncached engine.
`--test-server-protocol` is hardware-independent. Compact evidence is archived
under `data/ocr-performance-*/`; large temporary comparison captures are removed
after successful installation, while pre-existing user GUI runs are untouched.

## Serving CPU and accelerator profile

The next September 17 update targets the warm serving path rather than cache
creation. `export-glm-ocr.py --profile-serving` starts a native server, sends three
complete screenshot requests serially, excludes the first as warm-up, and records
wall/first-output time, engine-process user+kernel CPU seconds, logical process
read/write bytes, exact output hashes and all eight QNN graph phases. It waits on
the process handle between completion-file checks; no busy-spin load is used.
`--profile-reference PATH` requires exact agreement with an earlier completed
report. `--check-model-locks` also attempts write-open access without modifying
files and requires Windows sharing violations for all used model artifacts.
`--compare-serving --generation-build CANDIDATE --baseline-build BASELINE` combines
only completed reports matching each executable hash and the same input/output,
requires at least four warm samples each, checks all 40 cache hits, and verifies
zero warm decoder, final-normalization and head finalizations in the candidate.

Profiling found about 6.01 GB of logical reads per warm request, while registration
of Vision/prefill graphs cost only about 1.7 s. The engine now holds at most 64
read-only file handles for verified model artifacts during a server session,
allowing read sharing but denying writes and deletion. Initial full verification
is unchanged. A successful resident session reuses shared weights, embeddings,
tokenizer and existing decoder/head weight buffers rather than reloading and
revalidating all artifacts. Vision block reads still use the same held handles
and retain their artifact checks. Handles are released on server shutdown; to
replace model files, close their server first. Image inputs and mutable graph
cache files are not held in this table. The table costs about 4 MiB for bounded
UTF-16 paths and does not introduce another full weight copy.

`ocr_generate.c` now retains the final decoder RMSNorm graph alongside the
decoder and head, avoiding the former per-token context creation/finalization.
The ordinary non-reuse path remains available. All retained contexts are released
through the common shutdown path. Operations, weights, finite checks, precision,
KV reset, masks and generated-token selection are unchanged.

Two independent server runs per executable provide four warm requests each on
the user's unchanged `gui-compact.png`. The intermediate weight-reuse-only build
is excluded from the final aggregate. No concurrent agent inference or build ran
during timed requests. Clock/power/background-system load were not controlled;
baseline time varied 31.65..38.87 s and candidate time 26.78..29.23 s. Medians:

| Metric | Previous resident app | Updated resident app | Change |
| --- | ---: | ---: | ---: |
| Request wall time | 35.056 s | 28.710 s | -18.1% |
| First output | 24.903 s | 20.514 s | -17.6% |
| Engine CPU time | 17.977 CPU-s | 13.383 CPU-s | -25.6% |
| Mean CPU core equivalents | 0.522 | 0.462 | -11.4% |
| Logical read bytes | 6,010,273,186 | 2,809,957,765 | -53.2% |
| QNN accelerator time | 2.716 s | 2.636 s | -3.0% |
| Accelerator time / request time | 7.77% | 9.23% | +1.46 percentage points |

CPU core equivalents are CPU seconds divided by wall seconds, not a percentage
of all machine cores. CPU time excludes other processes and uncharged driver work.
The accelerator share is the median per-request ratio of QNN-reported accelerator
microseconds to wall time: it indicates less host-side waiting, **not measured NPU
hardware utilization, HMX occupancy or peak TOPS**. Absolute accelerator work stays
approximately constant, as expected for unchanged graphs. This is a small repeated
comparison, not a claim of steady-state production throughput or full NPU usage.

All token IDs through EOS, text and small state captures match the reference;
the model write locks and normal server cleanup pass. Native verifier/prefill/
generation checks and ARM64/no-CRT audits pass. The separate 896x896 PNG GUI test
matches its expected four lines: first 33.234 s, repeat 20.659 s; invalid input,
cancel/restart, close-active, missing engine and layouts pass. Model numerical
acceptance remains false. The next substantial targets are context loading and
diagnostic output transfers/fusion, not increasing CPU concurrency or submitting
extra NPU work simply to raise a utilization number.

Compact comparison/profile evidence and prior executables are preserved under
`data/ocr-serving-*/`. The executable-bound cache is replaced at installation,
not accumulated alongside the old version. Production remains native no-CRT C;
Python is used only for optional profiling and regression tests.

## Bounded grouped serving

The installed engine uses `-Generate -LargeImages -ReuseDecode -GraphCache
-AppMode`. It was validated in `build/ocr-grouped` and installed at the existing
`build/ocr-app` location, without changing the GUI executable or original weights.

Production Vision blocks expose only their final hidden state; the connector
exposes only image features. Text layers expose hidden state, K and V. Diagnostic
builds retain their intermediate outputs and probability checks. Production
still checks every returned element for finiteness. The original numerical
acceptance threshold is unchanged and remains unfulfilled.

Prefill uses 32-row buckets covering the actual prompt instead of always 256
rows. The causal mask is packed to that stride, and final normalization uses the
same row count. Decode capacity remains 256. Prefill writes K/V directly to the
host cache buffers; decode still uploads host K/V prefixes. This is not an
entirely device-resident KV implementation or fused transformer-layer graph.

Cache v5 groups two graphs per context: twelve Vision and eight prefill `.qob`
files. Each key binds executable/runtime hashes, verified model identities,
geometry and static RoPE/mask data. Checksums cover binding metadata and binary
payload. A cache hit restores the context before QNN tensor/node registration;
only CPU-side I/O descriptors are reconstructed. A damaged group is rebuilt on
HTP, never evaluated by a CPU neural fallback.

The first two Vision and first two prefill layers may remain resident. At most
one further group is active. Other groups are freed at the next group boundary;
memory pressure can also evict the retained groups. Model files remain locked
against writes for the session. Temporary decode/head weight copies are released
after successful first execution; a prefill rebuild reloads and verifies them.
Context binaries remain alive until their owning context is freed.

The failed all-resident experiment is not a supported configuration. With both
large bundles loaded, Decode layer 3 returned `0x1771` despite about 2.24 GiB
reported available physical memory. Host counters do not measure all HTP resource
limits. A separate bug counted decode RAM reserve against the binary-cache cap;
those checks are now separate. The candidate checks a 3 GiB binary cap, 12 GiB
private-process limit and 512 MiB physical reserve, with additional admission
headroom. These are admission checks, not an OS-enforced QNN memory quota.

Use `export-glm-ocr.py --test-graph-bundles` with separate staged candidate and
baseline directories for task/grid transitions and binding/payload/truncation
corruption. `--profile-serving --check-model-locks --profile-reference ...`
compares all small captured states, including K/V prefix hashes and token IDs.
Timing reports include process working set/private memory. Accelerator time
divided by wall time is not Task Manager utilization.

### Measured results (2026-09-17)

Serial comparisons of four warm baseline and six warm grouped requests used the
same screenshot and exact small-state/token/KV-hash comparison. The baseline
engine was `5140b0f3...`, the installed grouped engine `318a0882...`.

| Metric | Baseline median | Grouped median | Change |
| --- | ---: | ---: | ---: |
| Complete request | 25.780 s | 17.222 s | -33.2% |
| First text | 17.772 s | 9.316 s | -47.6% |
| Process CPU time | 11.406 s | 7.250 s | -36.4% |
| Logical reads | 2,809,957,785 B | 1,803,911,757 B | -35.8% |

Warm wall ranges were 21.904-28.075 s and 14.576-19.492 s. Clock, power and
background activity were not fixed. Fresh cached grouped processes took
37.14-37.39 s on their first request; creating an empty cache took 125.97 s.
These first requests are excluded from the warm medians. An earlier empty-cache
attempt exited during decode preparation without a captured exception code;
its cause was not conclusively established. The subsequent empty-cache test,
fresh-process tests, five task/grid transitions, three corruption recoveries,
GUI tests and installed smoke check completed successfully. This is development
regression evidence, not an exhaustive reliability or OCR-quality guarantee.

The five task/grid transitions cover Text, Formula, Table, a wide image and
return to the original image. All small states matched the previous engine.
Tensor-ID, payload and truncation corruption each rebuilt exactly one group
while restoring the other nineteen. Cache size was 1,831,658,304 bytes.
Seventeen malformed/EOF server-protocol cases and native no-CRT tests passed.
GUI recognition returned the expected four lines twice (23.627 s / 11.179 s),
with cancellation, restart, resize and active-window close tests passing.

The previous executables, comparison reports, corruption/transition evidence
and installed hashes are archived under
`data/ocr-serving-6e3b74d97ccb4618b64afc67df4606ef`. Installation verified twenty
direct group restores, forty graph bindings, the exact three-token prefix and
absence of large diagnostic captures. Original-reference numerical acceptance
still fails; no tolerance or precision relaxation was used to accept this update.

## Concrete image-to-text gaps

1. **Additional input formats.** BMP24 and bounded static PNG of all standard
   color/depth modes now work, including palette transparency and Adam7. JPEG,
   ICC color management, EXIF orientation and animation remain unsupported.
2. **Larger image-to-vision buckets.** The native PNG/BMP-to-HTP connection now
   supports four grids through 16x32 patches in the large build. Full-page grids,
   additional aspect ratios or tiling and their memory/numerical checks remain.
3. **Vision accuracy and lifecycle optimization.** All 24 blocks and postnorm now
   execute with checked handoffs and reference reports. Accumulated FP16 error
   remains unaccepted; bounded context caching is implemented, but lean graph
   outputs and avoiding repeated construction/weight loads remain useful work.
4. **Connector accuracy.** Learned 2x2 downsampling and the complete merger to
   1536-wide features now execute, but their full-chain numerical gate remains
   unaccepted. Their direct connection to text input is implemented above.
5. **Broader multimodal inputs.** Single-image fixed-task input assembly is now
   exact against the official template and model. Larger contexts, multi-image
   inputs and optional schema-driven extraction prompts remain unsupported.
6. **Text accuracy and device caching.** All 16 decoder layers and final norm now
   execute on HTP with checked positions, masks and handoffs. Resolve the remaining
   numerical differences; optional reusable decode/head graphs now exist, while
   device-resident KV updates remain work; Vision/prefill contexts now reload
   from a bounded disk cache.
7. **Broader decode validation.** Head, greedy selection, EOS/limits, UTF-8 and
   incremental KV updates are implemented for the bounded context. Cover larger
   contexts, richer prompts and multilingual generation beyond tokenizer fixtures.
8. **Serving lifecycle.** A single-image end-to-end command now exists with failure
   propagation and cleanup. The native GUI now provides tested process-based
   repeat/cancel behavior with resident decoder/head reuse, bounded disk caching
   and per-request KV isolation. Graceful cancellation and lean graph outputs
   remain; large tensor capture files are disabled in app mode.
9. **Acceptance and performance.** An independent end-to-end comparison now exists
   for the small and larger development images. Still needed: a held-out
   image/text corpus, digits/punctuation, omissions, repetition, reading order,
   multilingual text and termination; then matched-quality cold/resident timing.
   Initial graph timings and process memory are measured above, but no current
   OCR quality or latency acceptance exists.

PDF rasterization, multi-page orchestration and optional layout
analysis are extensions, not prerequisites for a single-image-to-text baseline.
The native testing GUI is implemented above.
MTP and low-bit quantization are also not needed for that baseline.

## Next stages

1. **Execution contract and tokenizer: completed above.** Initial source audit and
   bounded tokenizer are verified; image and numerical contracts remain below.
2. **RGB preprocessing and positions: completed above.** Native BMP24 file input
   now reaches reference-checked patches; common compressed formats and real scan
   fixtures remain integration work. Extend the numerical
   oracle to learned-layer taps before claiming a correct model forward pass.
   PDF rasterization is a separate feature, not an implicit external dependency.
3. **HTP capability and precision probes: primitives, weight audit, patch and block 0 completed above.** Continue with
   larger vision grids, accumulated block error, merger, text attention/mRoPE and KV
   updates. Test real dimensions, finite outputs and cleanup on the installed SDK.
   Weight casting ranges are audited; activation and accumulated errors remain open. No blind
   cast, silent clamping, assumed BF16 HTP support or premature W4 conversion.
4. **Vision encoder and connector: executable prototype above.** All blocks and
   connector run on two bounded grids; improve numerical acceptance, cover larger
   images and implement retained compiled contexts. No whole-page quality claim.
5. **Text prefill and decode implemented.** The 16-layer decoder has a
   64-row prefill and 256-position decode bucket, untied output head, RAM-resident KV, greedy selection,
   termination and UTF-8 streaming, with open precision gates. Config's
   131072 positions is NOT a tested runtime capacity. MTP is a later extension.
6. **Usable OCR and quality gates.** Image-to-text CLI first, then tables/formulas
   and optional separately pinned layout analysis. Evaluate German/English printed
   pages, multilingual text, digits, punctuation, omissions, repetition, termination,
   and ordering. Keep a held-out corpus; AI semantic review is allowed and attributed.
7. **Performance.** Matched quality-controlled cold/resident runs: image processing,
   vision, prefill, decode, CPU time, copies, context size and peak memory. Optimize
   batches, buckets, graph fusion and caching using those measurements.

Current status: source acquisition, verification, bounded native tokenizer/prompt
runtime, native BMP24-to-patch file input, RGB resize/normalization/patch packing,
single-image positions, isolated
FP16 HTP primitives, complete weight range audit, learned patch projection and
complete vision block 0 on three 64-patch grids and two visible 128-patch OCR
examples against two numerical oracles, plus a complete native 24-block vision
and connector forward path on 64/128 patches, exact multimodal prompt/embedding
assembly, integrated 16-layer text prefill, 256-position autoregressive generation,
optional retained decode/head graphs and an independent full-model text comparison
with native UTF-8 output. Structural/execution checks pass;
Vision and text numerical gates remain explicitly failing.
No full-model FP16 accuracy, PDF support or OCR quality
acceptance is claimed. No previous Whisper or TranslateGemma campaign is restarted.