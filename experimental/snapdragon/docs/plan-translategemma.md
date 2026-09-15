# TranslateGemma 4B direct-QNN implementation plan

## Objective

Run the text-only language backbone of `google/translategemma-4b-it` on the
Snapdragon X Elite HTP with useful interactive translation speed. The deployed
program remains freestanding ARM64 C, uses no CRT or standard C library, imports
only Windows system APIs statically, and calls QNN directly. Python, PyTorch, and
NumPy are permitted only for pinned model acquisition, conversion, calibration,
and reference generation.

The first release supports text-to-text translation. The SigLIP vision tower and
image translation are explicitly deferred.

## Feasibility baseline

The official checkpoint is a Gemma 3 conditional-generation model. Its text
backbone contains exactly 3,880,263,168 BF16 parameters; the complete stored
checkpoint has 4,300,079,472 parameters after including the vision tower and
projector. The text backbone uses:

| Property | TranslateGemma 4B text backbone |
| --- | ---: |
| Decoder layers | 34 |
| Hidden width | 2560 |
| MLP width | 10240 |
| Query heads | 8 |
| KV heads | 4 |
| Head width | 256 |
| Vocabulary | 262208 |
| Translation context | 2048 tokens |
| Attention pattern | Five local layers per global layer |
| Local window | 1024 tokens |

BF16 text weights require 7,760,526,336 bytes (about 7.23 GiB) before QNN context and runtime
overhead. That is not a practical deployment format on the target 16 GiB
machine. The production target is symmetric weight-only INT4 with FP16
activations (`W4A16`); INT8 is a bring-up and quality-control format.

At the 2048-token deployment context, an FP16 KV cache requires about 156 MiB
when the 29 local layers retain 1024 tokens and the five global layers retain
2048. KV storage is still not the primary memory risk; duplicated weights in
prompt and token-generator contexts are.

Comparable Qualcomm AI Hub 4B models already use `q4_0w4a16` on Snapdragon X
Elite. This establishes platform feasibility, but it is not a performance result
for TranslateGemma or for this direct-QNN implementation.

## Constraints

- Runtime code is C11, freestanding, no CRT, and no libc.
- `KERNEL32.dll` remains the only static DLL dependency of the inference tool.
- `QnnHtp.dll`, `libcdsprpc.dll`, HTP stubs/skels, and firmware remain unavoidable
  dynamically loaded platform dependencies.
- The model, tokenizer, and serialized QNN contexts are external validated data,
  not linked into the PE image.
- ONNX Runtime, Genie, llama.cpp, and Transformers may be reference tools but are
  not part of the deployed process.
- The model revision, original hashes, conversion settings, runtime version,
  graph contract, and generated artifact hashes are pinned.
- No proprietary Qualcomm headers or binaries are committed.
- Every optimization is accepted on end-to-end latency, memory, and translation
  quality, not NPU utilization alone.
- Fixed-shape graphs and model-specific context binaries are preferred over a
  general runtime graph interpreter.

## Non-goals

- Supporting the 12B or 27B variants in the first implementation.
- Supporting image translation or the SigLIP vision encoder.
- Reimplementing a general Safetensors, Jinja, or Hugging Face runtime in the
  deployed binary.
- Loading arbitrary Gemma-family checkpoints without conversion and validation.
- Matching every Transformers sampling option. Deterministic greedy generation
  is the initial contract.
- A full CPU implementation of the 4B model in the production executable.

## Stage 0: Source-tree cleanup

**Status: complete (2026-09-13).**

Reorganize `experimental/snapdragon/src` before adding another model. This stage
is a mechanical ownership change, not a runtime refactor.

Target layout:

```text
src/
  shared/
    imports/
      kernel32.def
      dxcore.def
      d3d12.def
      directml.def
    tests/
      qnn_mock.c
    qnn_abi.h
  tools/
    probe/
      main.c
    whisper/
      main.c
      whisper_artifact.c
      whisper_artifact.h
      whisper_decoder.c
      whisper_decoder.h
      whisper_decoder_qnn.c
      whisper_decoder_qnn.h
      whisper_encoder_qnn.c
      whisper_encoder_qnn.h
      whisper_frontend.c
      whisper_frontend.h
      whisper_model.c
      whisper_model.h
      benchmarks/
        decoder_kernel_benchmark.c
      tests/
        whisper_artifact_test.c
        whisper_decoder_cleanup_test.c
    gemma/
      ... added by later stages
```

Actions:

1. Move files with history-preserving renames and keep public executable names
   unchanged: `probe.exe`, `npu_probe.exe`, `npu_probe_builder.exe`, and
   `decoder_kernel_benchmark.exe`.
2. Move only the already model-independent QNN ABI, import definitions, and mock
   provider into `src/shared`. Keep `whisper_artifact.*` under Whisper because its
   current header embeds Whisper-specific dimensions and payload kinds.
3. Update `build.ps1`, `test-npu-probe.ps1`, exporter references, include paths,
   and documentation links.
4. Do not split `npu_probe.c`, rename symbols, change artifact formats, or extract
   new abstractions in the same change. Those edits would make regressions harder
   to attribute.
5. Reserve `src/tools/gemma` as the owner of all TranslateGemma-specific runtime
   code. The directory becomes concrete when Stage 2 adds its first model files;
   do not add an empty placeholder file.

Exit criteria:

- All four existing binaries build with unchanged names.
- The artifact and decoder cleanup tests and all mock-provider cases pass.
- The established 35-second Small transcript remains unchanged.
- `npu_probe.exe` remains ARM64, has no exception or CLR tables, and imports only
  `KERNEL32.dll`.
- No generated artifact or QNN context format changes.
- `git diff --check` passes.

Implementation record:

- The shared QNN ABI, import definitions, and mock provider now live under
   `src/shared`; the probe and Whisper implementation now live under `src/tools`.
- All four binaries retain their original names and pass a clean build.
- The artifact contract, decoder cleanup, and all mock-provider cases pass.
- A rebuilt Small context restores and executes on the HTP, and the focused
   35-second quiet transcript matches the established output.
- The production PE remains ARM64 with no exception or CLR tables and with
   `KERNEL32.dll` as its only static import.

## Stage 1: Shared QNN substrate and runtime upgrade

**Status: complete (2026-09-13).**

QAIRT 2.50/QNN core 2.39 is now the single validated working runtime. Qualcomm's
comparable 4B packages require QNN SDK 2.45 or newer, so this satisfies the SDK
floor without keeping a parallel legacy installation.

The official archive is hash-pinned, the narrow ABI and provider gate require
QNN core 2.39, the ARM64 HTP runtime is staged from that archive, all Whisper
contexts were regenerated, and the Small transcript gate passed.

Actions:

1. Acquire the exact SDK headers and runtime under their license and record the
   archive version and hashes outside Git.
2. Extend `src/shared/qnn_abi.h` from the exact headers. Preserve compiler-checked
   ARM64 structure sizes, offsets, enum values, and function-table slots.
3. Add narrowly shared QNN loader/lifecycle and context-cache helpers only after
   both Whisper and Gemma call sites are known. Keep graph construction
   model-specific.
4. Retest every existing Whisper graph and context under the upgraded runtime.
5. Add capability probes for the operations and data contracts needed by Gemma:
   W4A16 FullyConnected, RMSNorm, gated GELU, RoPE arithmetic, Gather, grouped
   query attention shapes, causal masking, shared-memory KV tensors, and
   ArgMax/TopK if their public ABI is available.

Exit criteria:

- Existing Whisper output and failure-path behavior remain accepted.
- The narrow ABI is derived from and checked against the pinned SDK.
- A model-shaped W4A16 projection passes a scalar or high-precision reference.
- Unsupported operations have an explicit host-side or graph-composition
  fallback before full-model work starts.

Implementation record:

- The capability suite is builder-only and runs with
   `tools/test-gemma-stage1.ps1`; the production `npu_probe.exe` neither links the
   probe module nor accepts its option.
- A representative `[1,2560] x [2560,2560]` FP16 activation/W4 projection agrees
   with the scalar reference. Native packed `QNN_DATATYPE_SFIXED_POINT_4` is rejected by this HTP
   provider. The accepted construction contract uses signed 8-bit build-time
   storage with `QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET`, bit width 4,
   axis 1, and per-output-channel scales. Production artifacts may remain packed
   W4 and are unpacked only while constructing the QNN context.
- Direct FP16 RMSNorm, RotaryEmbedding, Gather, Argmax, and TopK pass numerical
   checks. Gated GELU passes as `Gelu` followed by `ElementWiseMultiply`.
- The public `GroupQueryAttention` node accepts graph construction but the
   Windows HTP provider rejects it during finalization with error 1002. The
   explicit fallback expands each KV head for its query-head group and executes
   MatMul, causal-mask addition, Softmax, MatMul, Transpose, and Reshape on HTP.
   The model geometry of 8 query heads, 4 KV heads, and head width 256 passes a
   two-token causal numerical reference.
- Two FP16 KV tensors of shape `[1,4,2048,256]` are allocated in one `rpcmem`
   region, registered as separate QNN shared-buffer handles, consumed and
   produced by an HTP graph, and compared exactly after execution.
- Shared lifecycle extraction remains deferred until the Gemma runtime provides
   the second production call site. Stage 1 adds only the ABI required by the
   probes and keeps graph construction model-specific.

## Stage 2: Pinned model descriptor and acquisition

**Status: complete (2026-09-13).**

The descriptor, catalog, authenticated fetch path, strict configuration and
Safetensors validator, deterministic tensor audit, and local failure fixtures
are implemented. An authenticated official download under
`data/translategemma-4b` matches the pinned revision across all Git blob IDs,
LFS SHA-256 values, and file sizes. The complete checkpoint passes configuration,
index, shard-header, tensor-selection, and inventory validation. Its local
`source-lock.json` and `tensor-audit.json` are generated data and remain outside
Git with the licensed model files.

Materialize `src/tools/gemma` with a canonical descriptor for TranslateGemma 4B.

Actions:

1. Add `gemma_model.h/.c` with model identity, layer types, dimensions, vocabulary,
   context limit, RoPE parameters, normalization epsilon, activation, token IDs,
   and artifact limits.
2. Add a model catalog under `tools/` containing the accepted Hugging Face
   revision, expected files, sizes, and SHA-256 hashes.
3. Require the user to accept the Gemma license and provide authenticated access;
   never embed credentials in scripts or manifests.
4. Download atomically with resume support and verify every file before use.
5. Validate the checkpoint configuration against the compiled descriptor.
6. Select only language-model tensors for text translation. Record every omitted
   vision and projector tensor so omission is auditable.

Exit criteria:

- A fresh authenticated fetch is reproducible from the pinned catalog.
- Configuration drift, missing shards, unexpected tensors, truncation, and hash
  mismatch fail before conversion.
- The exact retained parameter count and raw byte count are recorded.

Implementation record:

- `src/tools/gemma/gemma_model.h/.c` pins the 34-layer, 2560-wide text descriptor,
   the 5-local/1-global schedule, and the immutable source revision.
- `tools/translategemma-models.json` records all 15 official files with complete
   SHA-256 pins. Git-stored files also retain their official blob SHA-1 identities,
   while LFS files retain their official object SHA-256 identities.
- `tools/fetch-translategemma.ps1` requires `-AcceptGemmaLicense`, reads a token
   only from the environment or standard Hugging Face cache, verifies the exact
   revision, resumes into a staging directory, and publishes only after validation.
- `tools/validate-translategemma.py` rejects catalog, file, configuration,
   index, shard-header, namespace, shape, offset, and inventory drift without
   loading tensor payloads into memory.
- The source shard headers contain 883 tensors. Text selection retains 444
   `language_model.*` tensors totaling 3,880,263,168 parameters and 7,760,526,336
   raw bytes. It reports and omits 439 `vision_tower.*` or
   `multi_modal_projector.*` tensors totaling 419,816,304 parameters and
   839,632,608 raw bytes.
- `tools/test-gemma-stage2.ps1` runs the descriptor and acquisition gates;
   its `test-translategemma-stage2.py` suite covers accepted selection, configuration
   drift, file corruption, truncation, shard-map drift, unexpected namespaces,
   and missing files using local fixtures.

## Stage 3: Versioned artifacts and W4A16 conversion

**Status: complete (2026-09-13).**

Create a Gemma-specific artifact contract rather than extending Whisper headers
with unrelated fields.

Artifact classes covered by the contract:

- W4 language-model weights and per-group scales.
- FP16 normalization and other small parameters where quantization is not useful.
- Pinned tokenizer source data and, in Stage 4, generated vocabulary,
   normalization, byte-mapping, and lookup tables.
- Layer-type and RoPE tables.
- Deterministic prompt, layer-output, logits, and generated-token fixtures from
   Stage 5.
- Prompt-processor and token-generator QNN context binaries from Stages 7 and 8.

Actions:

1. Export from the original BF16 checkpoint, not a third-party GGUF conversion.
2. Start with W8A16 as a numerical bring-up control, then implement symmetric
   W4A16 using the exact grouping and packing accepted by HTP.
3. Record deterministic per-tensor quantization error during conversion. Stage 5
   applies the multilingual translation quality gate to the resulting W8 and W4
   artifacts.
4. Use a little-endian versioned header with model identity, source revision,
   tensor name or stable ID, shape, layout, element and quantization types, group
   size, payload size, and a strong payload hash.
5. Publish complete artifact sets atomically through a manifest written last.

Exit criteria:

- Every tensor round-trips through the artifact reader and agrees with the
  exporter reference.
- Wrong model, version, dimensions, layout, quantization, truncation, overflow,
  and payload corruption are rejected before QNN binding.
- W4 weight and scale storage is in the expected 1.9-2.2 GB range for the text model.

Implementation record:

- `src/tools/gemma/gemma_artifact.h/.c` defines a 256-byte little-endian v1
   per-artifact header with model and tensor identity, rank and dimensions,
   layout, element and quantization types, grouping, byte ranges, and SHA-256.
   The reader rejects reserved-field, model, name, dimension, overflow, size, and
   payload corruption before binding and expands low-nibble-first signed W4 into
   the QNN S8 build container.
- `tools/export-translategemma.py` revalidates the pinned Stage 2 checkpoint,
   streams BF16 tensors in bounded row chunks, uses ties-to-even symmetric
   quantization, records per-tensor RMSE and maximum absolute error, and writes
   the manifest only after every artifact has been reread and SHA-256 checked.
- QAIRT mapped group-128 encoding finalized and executed but returned an
   unscaled integer dot product; its float-block alternative failed inside QNN
   finalization. The deployed contract therefore uses the measured working path:
   one scale per output channel, S8 build storage with bit-width 4 and axis 1.
   The corrected 2560-wide hardware probe passes this contract on HTP.
- The published ignored artifact set under `models/translategemma-4b-stage3/`
   contains 444 W8A16 tensors with 3,883,175,040 payload bytes and 444 W4A16
   tensors with 1,943,227,520 payload bytes. W4 matrices and their scales occupy
   1,942,491,264 bytes; rank-1 FP16 tensors occupy 736,256 bytes.
- Six pinned tokenizer files, the 34-byte local/global layer schedule, RoPE
   constants, and the quantization contract are wrapped in the same format.
   Runtime-ready tokenizer tables, numerical model fixtures, and QNN contexts are
   deliberately named as deferred outputs in the manifest and are produced by
   their owning later stages rather than fabricated here.
- `tools/test-gemma-stage3.ps1` runs the C reader/corruption/unpack checks and
   Python streaming quantization tests. `tools/export-translategemma.py
   --verify-only` independently verifies the complete published inventory.

## Stage 4: Freestanding tokenizer and prompt contract

**Status: complete (2026-09-15).**

Implement only the tokenizer and template behavior required for TranslateGemma.

Actions:

1. Convert the pinned BPE vocabulary, merge ranks, added tokens, and language
   table into a bounded binary representation suitable for direct C lookup.
   The pinned fast tokenizer uses BPE, not unigram segmentation; its JSON and
   tokenizer configuration define the reference contract. Parse neither JSON nor
   protobuf in the deployed C code.
2. Implement UTF-8 validation, normalization required by the pinned tokenizer,
   ranked BPE merging, byte fallback, special tokens, and detokenization.
3. Implement the fixed TranslateGemma text prompt directly in C from validated
   source and target language identifiers. Do not embed a Jinja interpreter.
4. Reject unsupported language identifiers and inputs that would exceed the
   2048-token deployment contract, including the prompt and reserved output budget.
5. Compare token IDs and decoded bytes against the pinned Transformers tokenizer
   for ASCII, multilingual scripts, combining characters, malformed UTF-8,
   punctuation, and byte-fallback cases.

Exit criteria:

- Token IDs are identical to the reference corpus.
- Detokenized bytes are identical and valid UTF-8.
- Tokenization has deterministic allocation bounds and no CRT imports.

Implementation record:

- `src/tools/gemma/gemma_tokenizer.h/.c` provides an allocation-free table reader,
   heap-based BPE encoder, UTF-8 decoder, and fixed text prompt builder. The
   immutable table payload is 14,790,893 bytes; the caller-owned workspace is
   5,701,636 bytes. Input bytes are capped at 196608 and output tokens at 2048.
- The tokenizer defines 262145 IDs, while the model has 262208 output slots.
   Decoding unmapped IDs fails. Source spaces normalize to U+2581; there is no
   extra NFC/NFKC transformation. Added tokens are recognized before normalization.
   Configured extra-special-token flags match the pinned Transformers loader.
- Prompt generation supports the source template's 581 exact language identifiers,
   underscore-to-hyphen normalization, Unicode whitespace trimming, and exact
   prompt wording. The prompt token count plus requested output budget must fit
   the deployment context. Malformed input UTF-8 is rejected; invalid generated
   byte-fallback runs decode to the same replacement bytes as the reference.
- `gemma_model_is_stop_token` recognizes both 1 (`<eos>`) and 106
   (`<end_of_turn>`). Greedy reference generation must explicitly disable the
   source generation configuration's sampling default.
- The existing `tools/export-translategemma.py --tokenizer-only` path exports
   tables using only Python's standard library. `--tokenizer-reference` additionally
   requires pinned Transformers 4.57.3, Tokenizers 0.22.2, and Jinja 3.1.6 to
   regenerate fixtures. Both artifacts are reread and hashed before an atomic
   manifest-last publication under `models/translategemma-4b-stage4/`.
- `tools/build-gemma.ps1 -Test` builds and executes the freestanding ARM64 C
   runner without Python or QNN. All 7470 reference cases pass, covering every
   added token, every supported language code, multilingual and randomized text,
   whitespace, byte fallback, malformed input, and context/output bounds. Eighteen
   artifact corruption checks pass, including malformed tables with recomputed hashes.
- The build automatically audits the PE: Kernel32 is its only DLL import, with
   no exception or CLR tables. Stage 2 and Stage 3 regressions pass, and the full
   published Stage 3 W4/W8 artifact inventory was rehashed successfully.
- This stage supplies the tokenizer library and test executable, not a working
   translator. Numerical model references and QNN transformer blocks remain the
   next work in Stages 5 and 6. Normal builds consume existing artifacts; Python
   is neither a build prerequisite nor an inference dependency.

## Stage 5: Minimal numerical reference

**Status: RoPE and scaled-residual contract resolved; W4 generation/quality
remains blocked by degraded output and a token-limit failure (2026-09-15).**

Build a development-time reference that isolates model correctness from QNN
integration. It may use Python/Transformers, but all fixtures consumed by the C
runtime are immutable and hashed.

Actions:

1. Record BF16 outputs for embedding, RMSNorm, RoPE, grouped-query attention,
   gated GELU MLP, one complete local layer, one complete global layer, final
   normalization, and vocabulary projection.
2. Record W8A16 and W4A16 simulated outputs using the exact deployment packing and
   rounding rules.
3. Record greedy token sequences for a small multilingual translation corpus,
   explicitly setting `do_sample=False` and honoring both stop-token IDs.
4. Add freestanding scalar checks for packing, dequantization, RoPE, mask creation,
   KV indexing, and argmax tie-breaking. Do not implement a production-speed CPU
   copy of the complete model.

Exit criteria:

- Primitive and layer fixtures identify whether differences originate in export,
  quantization, graph composition, or generation policy.
- Greedy BF16 output is reproducible for every acceptance sentence.

Implementation contract:

- `tools/translategemma-reference.py` is an offline reference module invoked by
   the existing exporter with `--numerical-reference`. It is not linked into or
   needed by a deployed executable. Native PyTorch is unavailable for the local
   Windows ARM64 Python 3.14 environment; NumPy 2.4.3 provides the native BLAS
   reference, with bounded row-wise weight conversion rather than a full FP32
   copy of the model.
- BF16 simulation reads original checkpoint weights, accumulates matrix products
   in FP32, and rounds operation outputs to BF16 using ties-to-even. W8/W4 simulation
   reads the actual Stage 3 packed weights and stored FP16 scales, accumulates in
   FP32, and rounds activation outputs to FP16. These are explicit numerical
   reference semantics, not claims of bit-identical PyTorch or HTP accumulation.
- Recorded traces include embedding scaling, input/Q/K RMSNorm, Q/K/V projections,
   split-half RoPE, visibility masks, attention scores and probabilities, grouped
   attention output, residuals, gated GELU, complete local layer 0 and global layer
   5, final RMSNorm, and the tied vocabulary projection. Sparse absolute positions
   0, 1023, and 1024 exercise the local-window boundary. Array shapes, byte types,
   semantic precision, model identity, and SHA-256 are recorded in the manifest.
- **RoPE contract, version 2:** the checkpoint's `rope_parameters` is
   authoritative: global theta 1000000 with linear factor 8, local theta 10000
   with factor 1. Divide global inverse frequencies by 8 before multiplying by
   absolute positions. Stock Transformers 5.17.0/PyTorch 2.14.0 confirms this
   interpretation, including positions 0, 1, 1023, 1024, and 2047. Stage 3's
   `metadata/rope` now agrees with execution. The earlier schema-1 reference
   reproduced Transformers 4.57.3 ignoring this field; its factor-1 artifacts
   remain historical diagnostics and are rejected by current verification.
- **Residual contract, version 2:** BF16 remains unscaled. W8/W4 store the
   residual stream as `hidden / 32` in FP16. Divide the embedding multiplier and
   post-attention/post-MLP RMSNorm gains by 32 *before* casting to FP16. Input,
   pre-MLP, and final RMSNorm use epsilon `1e-6 / 1024 = 9.765625e-10` and their
   original gains. Q/K RMSNorm, projections, attention, and KV remain unscaled.
   This preserves the real-arithmetic model, not identical finite-precision
   rounding. Do not cast an unscaled branch first, or change epsilon only after
   FP16 conversion. The vocabulary projection receives the final unscaled norm.
   Cross-variant residual-output errors are reported after multiplying W8/W4
   stored outputs by 32; logits are compared directly.
- The acceptance corpus includes the official model card's Czech-to-German
   example, English-to-Japanese, and German-to-English with a time expression.
   Messages use only a User role with exactly one text content entry containing
   `type`, `source_lang_code`, `target_lang_code`, and the text to translate.
   `apply_chat_template(..., add_generation_prompt=True)` supplies the Assistant
   prefix. Greedy decoding honors IDs 1 and 106 and decodes only newly generated
   tokens, as in the model card's direct-initialization example; image input and
   unsupported alternative prompting remain out of scope.
- `src/tools/gemma/gemma_numeric.h/.c` supplies only small allocation-free scalar
   primitives, not a production CPU transformer: FP16 conversion, signed W4/W8
   dequantization, split-half rotation, causal/local visibility, per-layer
   `[head,slot,channel]` KV element offsets, and first-maximum argmax. Invalid
   dimensions/positions/scales and nonfinite logits are rejected. Byte offsets
   for FP16 KV storage are twice the returned element offsets; local slots wrap
   at 1024 and global slots at 2048, with absolute positions retained for masking.
- `build-gemma.ps1 -TestNumerics` adds the independent hashed scalar fixtures to
   the existing C runner and PE audit. Ordinary builds/tests still need no Python,
   NumPy, Transformers, or QNN. `-ExportNumerics` explicitly regenerates the full
   offline corpus. `--primitives-only` exports a deliberately incomplete scalar
   set for fast checks and never labels Stage 5 complete.
- This three-sentence corpus is a numerical/generation regression gate, not the
   Stage 10 multilingual quality evaluation. W4/W8 changes are reported against
   BF16, not accepted by an invented quality threshold. The traces do not prove
   HTP accuracy or performance; those require the Stage 6 hardware comparison.
- A finite FP32 value outside FP16 range is a recorded numerical failure, never
   clipped or silently promoted to another deployment precision. Quantized
   attempts preserve the failing pre-cast FP32 activation, prompt IDs, layer and
   operation, and error. Such a case has no invented generated sequence. The
   manifest's `complete` flag denotes coverage of reference generation, while
   `quantized_generation_ready` separately requires every translation to finish.
   A quantized response exhausting 64 tokens is retained with its actual IDs,
   decoded bytes, residual maximum, and `status=token_limit`; readiness stays
   false. BF16 must still terminate and reproduce exactly. No token-budget
   extension or quality waiver is applied to repetitive output.
   **Root-cause confirmation:** stock upstream decoder layers independently
   reproduce the Czech prompt's unscaled layer-5 MLP residual overflow at W8
   73520 and W4 71568. BF16 stays finite. The optional x64 Windows oracle runs
   under emulation, isolated from native ARM64 calibration; it is development
   tooling only. Its stock Linear additionally rounds dequantized weights to
   FP16, so this is independent failure reproduction, not HTP bit parity.
   The full-depth BF16 prompt peak is 296224. Divisor 32 gives more than fourfold
   headroom against 65504; scaled W8/W4 prompt peaks are 9344.28125/8977.46875.
   All three prompts reach finite vocabulary logits. This measured corpus bound
   is not a proof for every possible 2048-token input.
- The optional `--residual-bound-audit` scans every mapped embedding row and
   bounds each post-norm channel by `sqrt(2560) * abs(1 + weight) / 32` across
   all 68 residual additions. Including FP16 relative rounding and subnormal
   allowances, the envelopes are W8 34446.264 and W4 34446.281, below 65504.
   This input-independent residual bound assumes finite branch inputs and
   exact-real RMSNorm; FP32/HTP normalization error and projection overflow
   remain outside its scope. The report is preserved under
   `models/translategemma-residual-bound-audit.json`.
- HTP core 2.39 / QAIRT 2.50 accepts FP32 add IO but does not preserve a residual
   sum of +/-73728. Wider IO types alone are therefore not a fix. The model-width
   `[3,2560]` FP16 add/RMSNorm probe passes with divisor 32, including a large
   residual, epsilon-sensitive small values, and zero. These are capability
   checks; Stage 6 must still compare actual folded norm gains and full blocks.
- The existing `test-translategemma-stage3.py` has optional `--torch-oracle` and
   `--residual-audit` modes. The former pins torch 2.14.0 and Transformers 5.17.0;
   the latter uses native NumPy and the existing tokenizer environment. Reports
   live in ignored `models/translategemma-precision-oracle.json` and
   `models/translategemma-residual-audit.json`. The x64 oracle report records
   the upstream module hash, checkpoint configuration hash, and exact prompt IDs.
- A separate `--residual-rounding-audit` compares W4's Japanese prompt with
   scaled FP16 versus diagnostic FP32 residual/branch outputs, keeping the same
   quantized projection weights. Both select first tokens `[220844,37307]`;
   the wide stream peaks at 287257.65625. The observed early generation
   divergence therefore persists without scaled FP16 residual storage. This
   two-token comparison does not claim identical complete responses or solve
   quantization quality. No production FP32/CPU fallback is introduced.

Validation record:

- The version-2 set publishes 184 verified artifacts. All three BF16 sequences
   replay exactly, and W8 matches all three BF16 token sequences byte for byte
   (18/4/15 tokens including stop). All nine attempts remain numerically finite.
   W4 terminates the Czech and German examples with changed text, but its
   Japanese case reaches the token limit: `complete=true` denotes diagnostic
   coverage, while `quantized_generation_ready=false` correctly remains set.
   Recorded residual maxima are BF16 296224, W8 9344.28125, W4 8977.46875.
   The final generation run took 3246 seconds; no timing claim for inference
   follows from this offline reference run. All artifact hashes/inventory,
   12 Python regressions, 323 C scalar cases, 7470 tokenizer cases, 18 corruption
   cases, and the Kernel32-only ARM64 PE audit pass after publication.
- Version 2 eliminates the observed residual overflow without clipping or a
   CPU fallback. W4 still gives degraded Czech-to-German wording and repeats a
   romanized greeting in the Japanese case until the 64-token limit. The actual
   response is retained as a failed generation diagnostic, not a translation
   acceptance. Continue Stage 6 with W8 first; investigate W4 quantization
   quality separately rather than changing RoPE back or relaxing stop criteria.
- The historical ignored set under `models/translategemma-4b-stage5/` contains
   184 validated artifacts, including all three variants' primitive and complete
   local/global layer traces, BF16 prompt/generated-token sequences, and six
   quantified FP16 overflow diagnostics. `quantized_generation_ready=false` is
   intentional: no W8/W4 full-model token sequences are claimed.
- In that historical set, all three BF16 translations reproduced exactly on a second full cached run.
   The manifest stores the exact Czech-to-German, English-to-Japanese, and
   German-to-English translation bytes, hashes, and 18/4/15-token sequences
   respectively, including each end-of-turn token. The time example produces
   "The train is scheduled to arrive at 3:30 PM."
- The freestanding C runner passes 323 numerical cases, including all 65536
   FP16 bit patterns, plus the existing 7470 tokenizer and 18 corruption cases.
   ARM64/Kernel32-only imports and empty exception/CLR tables pass the PE audit.
- Stage 2's 12 tests passed at initial publication; the expanded Stage 3/reference
   suite's 12 tests pass after the contract correction and token-limit handling.
   Independent checks cover RMSNorm, closed-form grouped attention, sparse
   local/global mask boundaries, cached-versus-full attention, actual W4/W8
   payload mappings, diagnostic preservation, and malformed array metadata.
- Source and all Stage 3 W4/W8 payload hashes were revalidated before generation;
   every numerical artifact and the complete output inventory pass read-only
   verification at initial publication. New artifacts use schema 2 under
   `models/translategemma-4b-stage5-v2/`; both Python verification and the
   Python-free C build entry point reject old or conflicting execution contracts.
   Neither diagnostic coverage nor finite generation is Stage 10 quality acceptance.

## Stage 6: QNN transformer block

Prove one local and one global decoder block before constructing all 34 layers.

Status: implemented and hardware-validated on 2026-09-15, Snapdragon X Elite,
QAIRT 2.50 / QNN core 2.39. Both W8A16 and W4A16 pass local layer 0 and global
layer 5 block gates. This is block arithmetic acceptance, not W4 translation
quality acceptance or a full-depth prompt/decode implementation.

Actions:

1. Build W8A16 graphs first and compare all meaningful intermediate outputs.
2. Replace projections with W4A16 while retaining the version-2 FP16 residual
   stream (`hidden / 32`), compensated normalization, attention, and KV storage.
3. Fold RoPE into Q/K preparation and use four KV heads with eight query heads.
4. Supply a fixed-size causal mask and a runtime position input. Avoid rebuilding
   or finalizing graphs per position.
5. Bind KV storage through registered FastRPC shared memory and verify offset,
   alignment, lifetime, and cache visibility explicitly.
6. Measure graph creation, finalization, first execution, warmed execution, DDR
   traffic when available, and numerical error.

Exit criteria:

- Local and global blocks pass their W4A16 numerical gates.
- Repeated execution updates or consumes the intended KV rows without copying a
  complete cache through ordinary application buffers.
- The warmed block result is deterministic and all failure paths release handles
  in reverse order.

Implementation and reproducible gate:

```powershell
.\experimental\snapdragon\tools\build-gemma.ps1 -TestBlocks -TestNumerics
```

The tool-private `src/tools/gemma/gemma_block.h/.c` builds a fixed three-token
graph with runtime positions and masks, in-graph split-half RoPE, four KV heads,
eight query heads, 2048 past slots, and three current slots. It retains the v2
scaled residual contract. W8 needs QNN's standard axis-scale-offset encoding;
the bit-width encoding is rejected for W8 on this backend. W4 uses the proven
S8 container with bit width 4 and output-channel scales. Stored matrix metadata
declares quantization axis 1; both variants are transposed for QNN MatMul.

The dedicated Kernel32-only ARM64 runner validates artifact identity and SHA-256,
then creates/finalizes once per block. It registers four page-aligned regions in
one FastRPC allocation: past K, past V, current K, and current V. Guard padding
and read-only cache fingerprints are checked after every execution. Only the new
three-token rows are copied inside shared storage, 12288 bytes per step; no
complete cache travels through ordinary application buffers. Local slots wrap
modulo 1024; global slots use absolute positions below 2048. Concat/head expansion
still materializes tensors inside HTP; this is not a zero-DDR production graph.

The offline fixture set contains 209 hashed artifacts, generated through
`export-translategemma.py --block-reference` into
`models/translategemma-4b-stage6/`. It uses actual W8/W4 weights and the existing
NumPy 2.4.3 reference, without full-model generation. Both layers receive the
same three embedded token IDs `[2, 9259, 1902]`, at positions `[0, 1023, 1024]`
then `[1025, 1026, 2047]` with cached K/V. Layer 5 is intentionally tested in
isolation, not with layer 4's output. `complete=false` describes the absence of
the full Stage 5 corpus in this block-only publication, not a failed block gate.

Twenty meaningful taps per step are compared, including visible attention scores
mapped from physical cache slots and softmax probabilities. All masked
probabilities must be zero. The fixed gate requires
`sum(error^2) <= 0.000625 * sum(reference^2) + 0.000001 * element_count`
and `max_abs_error <= 0.15 * max_abs_reference + 0.02`, plus finite outputs.
Thus the relative RMSE budget is 2.5% with a 0.001 absolute RMS floor. All observed
tap relative MSEs were below 19 ppm (relative RMSE below 0.44%); final block
outputs were below 3 ppm. Folded FP16 norm gains and backend GELU/softmax arithmetic
are tolerance-checked against the FP32-accumulating, explicitly rounded oracle,
not asserted to be kernel-bit-identical.

Each step has three warm replays with matching fingerprints for every tapped
output. Masking out past rows must change attention; restoring the mask must
reproduce it. Five injected failures after context creation, graph construction,
finalization, partial registration, and execution all return failure with zero
cleanup errors. Registered handles are released before their backing allocation;
the context is freed before weight/tensor storage, then device/backend/log and
loaded modules are released. FastRPC's free API itself has no status return.

Measured final gate (construction includes artifact reads, hashing and transpose;
warm median covers six replays, no power-mode control):

| Block | Construction s | Finalize s | First ms | Warm Median ms |
|---|---:|---:|---:|---:|
| W8 local 0 | 1.181 | 2.940 | 16.49 | 11.30 |
| W8 global 5 | 1.031 | 2.802 | 16.58 | 12.11 |
| W4 local 0 | 0.820 | 2.629 | 14.09 | 10.19 |
| W4 global 5 | 0.839 | 2.609 | 14.52 | 10.29 |

Logs and binding files are under `build/gemma-block/`; the runner has 74 graph
tensors with diagnostic outputs retained. DDR traffic was not measured because
no hardware traffic counter was requested. These figures do not establish the
Stage 7/8 performance targets. Full/deep caches, all-layer composition, prompt
padding, serialized restore, and full-model HTP generation remain later gates.
The production Whisper executable/context and historical reference sets are
unchanged. The build also passes tokenizer 7470, corruption 18, numeric scalar
323, and ARM64 no-CRT PE audits.

## Stage 7: Prompt processor

Create a fixed-shape prompt path that amortizes weight traffic across many input
tokens.

Status (2026-09-15): implementation in progress, hardware execution blocked.
The shape-aware block builder supports 128-token chunks and 512/1024/2048 buckets,
unique layer names, internal hidden connections, and final-token vocabulary
projection. The existing no-CRT runner now composes all 34 W4 layers into one
graph. The 512 bucket constructed in 28.50 seconds, finalized in 126.05 seconds,
and serialized to 1966771776 QNN bytes plus an application envelope. Disk restore
passed in 1.08 seconds. Construction-only weight buffers are released before
restore; no BF16/W8 artifact variant is loaded for this path.

Execution of the restored graph fails on this HTP runtime with
`Dma execution failed on the skel side`, result 1100, transport error 0.
Restore-only replay reproduces it. A temporary diagnostic using raw views of
the same shared buffers also returned 1100, so the failure is not confined to
registered-memory bindings. That retry was removed; there is no silent fallback.
Cleanup reports zero errors. The cause is not yet established: graph size,
128-token operations, and the large vocabulary projection require isolation
with smaller composed graphs before choosing a measured partition count.

Reproduce the current failure (not an acceptance command):

```powershell
.\experimental\snapdragon\tools\build-gemma.ps1 -TestPrompt -PromptBucket 512
.\experimental\snapdragon\tools\build-gemma.ps1 -TestPrompt -RestorePrompt -PromptBucket 512
```

The first command builds and stores `build/gemma-block/prompt-512.gmb.context`;
the second requires that context and avoids rebuilding/finalizing the graph.
The envelope retains graph/tensor names and IDs, dimensions, model repository
and pinned revision, W4/chunk/bucket metadata, QNN core/backend versions, and
SHA-256 covering metadata and binary. Fresh-process restore is exercised, but
corruption/malformed-envelope tests remain pending.

Offline `export-translategemma.py --prompt-reference` publishes 77 verified
artifacts under `models/translategemma-4b-stage7/`: full-depth W4 K/V for a
256-token deterministic sequence, embeddings, local/global RoPE tables, and
logits at tokens 253 and 256. Earlier positions of the full causal run provide
the unpadded 253-token reference. The runner's registered K/V output path,
253-token padded comparison, all-layer retained-row/logit checks, deterministic
256-token repeats, and 300 tokens/s gate are implemented but **not hardware
validated**, because execution fails before producing outputs. Larger buckets
have not been finalized or executed. Stage 7 exit criteria remain unmet.

Existing Stage 6 local/global W8/W4 hardware checks, five failure-cleanup probes,
7470 tokenizer cases, 18 corruption cases, 323 numerical scalars, and no-CRT PE
audits still pass after the builder changes. Twelve Python reference tests pass.

Actions:

1. Begin with 128-token chunks and context buckets of 512, 1024, and 2048 tokens.
2. Compose all 34 layers into one graph where QNN finalization permits it. If the
   graph compiler requires partitioning, use the smallest measured partition
   count and never default to one submission per layer.
3. Produce packed K/V rows directly into registered shared memory.
4. Mask padding so the final short chunk is numerically identical to an unpadded
   reference.
5. Serialize the finalized prompt context with an application header containing
   graph names, tensor IDs, model identity, shape bucket, QNN version, and hash.

Exit criteria:

- Prompt logits and every retained KV row pass reference tolerances.
- A 256-token prompt reaches at least 300 input tokens/s after warm restore.
- Prompt processing does not retain an unnecessary BF16 or W8 copy of W4 weights.

## Stage 8: Token generator and persistent KV state

The token generator is the decisive performance stage. It must execute one token
with one QNN submission, or with a very small fixed number justified by a faster
end-to-end result.

Actions:

1. Build a one-token graph containing all 34 layers, final RMSNorm, and the tied
   vocabulary projection.
2. Keep local and global KV caches in registered shared memory for the complete
   request. The graph consumes prior rows and emits the current packed K/V rows;
   a bounded host copy of only those new rows is acceptable if in-graph state
   update is unavailable.
3. Pass position, RoPE values, and masks as small inputs. Do not build one graph
   per token position.
4. Prefer a verified in-graph ArgMax for greedy decoding. Returning 262208 FP16
   logits for host argmax remains a correctness fallback and costs only about
   512 KiB per generated token.
5. Evaluate separate prompt and decode contexts to avoid simultaneous duplicated
   weights on the 16 GiB machine. Measure context-switch cost before deciding
   whether both can remain resident.
6. Serialize and restore the final graph; production execution must never rebuild
   nodes from model weights.

Exit criteria:

- Generated token IDs match the W4A16 reference.
- Warm decoding reaches at least 10 tokens/s at 512- and 2048-token contexts;
  15 tokens/s is the target.
- No per-layer host dispatch and no complete KV-cache copy occurs per token.
- No individual QNN call stalls above the documented acceptance threshold in a
  sustained translation run.

## Stage 9: End-to-end translation tool

Add a dedicated executable rather than adding Gemma modes to `npu_probe.exe`.

Proposed interface:

```text
translategemma.exe --from=cs --to=de-DE [--max-tokens=256] [--quiet] < input.txt
```

Actions:

1. Keep stdin/stdout UTF-8 and provide file arguments only if needed by measured
   workflows.
2. Restore QNN contexts once and process multiple requests in one process.
3. Implement deterministic greedy generation with EOS and maximum-token bounds.
4. Report model/context identity, token counts, prompt and decode rates, time to
   first token, peak memory, and QNN call statistics outside quiet mode.
5. Stream decoded output without emitting invalid or incomplete UTF-8 sequences.
6. Use distinct builder and runtime executables if graph construction materially
   increases production code or data size, following the Whisper precedent.

Exit criteria:

- A clean process translates every acceptance sentence and exits without leaked
  QNN, file, memory, or thread resources.
- Quiet output contains only translated UTF-8 text.
- The runtime binary has no CRT, exception, or CLR tables and statically imports
  only `KERNEL32.dll`.

## Stage 10: Quality, performance, and memory gates

Use a fixed multilingual corpus with short sentences, long paragraphs, difficult
scripts, named entities, numbers, markup-like text, and near-context-limit input.

Required gates:

| Metric | Minimum gate | Target |
| --- | ---: | ---: |
| Warm decode, 512-token context | 10 tokens/s | 15+ tokens/s |
| Warm decode, 2048-token context | 10 tokens/s | 15+ tokens/s |
| 256-token prompt prefill | 300 tokens/s | 500+ tokens/s |
| 256-token prompt TTFT | under 2.0 s | under 1.0 s |
| Peak working set | under 11 GiB | under 8 GiB |
| Static PE imports | Kernel32 only | Kernel32 only |
| Malformed UTF-8 output | zero | zero |

Quality gates:

- Compare W4A16 against the original BF16 model, not only against a community
  quantization.
- Record exact greedy token agreement for deterministic fixtures.
- Measure COMET, chrF, or another pinned translation metric on a representative
  subset of the 55 evaluation languages.
- Set the acceptable W4 quality delta only after recording W8 and BF16 baselines;
  do not select clipping or group size from a single language pair.
- Review every changed translation in the small fixed regression corpus.

Benchmark reporting:

- Separate cold process, runtime load, context restore, tokenization, prompt
  prefill, first token, warm decode, detokenization, and cleanup.
- Report median, p95, maximum, and calls over 10/100/1000 ms.
- Report process CPU time, peak/current working set, private committed bytes, QNN
  host-call duty, generated tokens, and exact output hash.
- Use repeated interleaved runs and record Windows power mode because the existing
  Whisper measurements show substantial scheduling and power-state variance.

## Stage 11: Packaging and hardening

Actions:

1. Define a standalone directory layout containing the executable, pinned QNN
   runtime, model artifacts, context binaries, manifest, and required licenses.
2. Resolve resources relative to the executable or an explicit `--model-dir`;
   do not depend on the repository working directory.
3. Add deterministic tests for malformed CLI input, unsupported languages,
   missing runtime, provider/API mismatch, wrong context, damaged artifacts,
   allocation failures, QNN execution failures, output overflow, and cleanup.
4. Add an automated LLVM PE audit to the test runner.
5. Record model and QNN redistribution obligations. Quantized model artifacts
   remain governed by the Gemma terms.
6. Document exact clean-room reproduction from accepted model access through
   export, context build, tests, and deployment staging.

Exit criteria:

- The packaged directory runs outside the repository checkout.
- No Python, PowerShell, ONNX Runtime, Genie, Visual C++ runtime, or model-converter
  component is needed for inference.
- A fresh artifact corruption and QNN mock run proves all documented failures.

## Stop and reconsider conditions

Stop full-model implementation and revisit the design if any of these occur:

- The target HTP runtime cannot execute a numerically acceptable W4A16 projection.
- Prompt and token graphs require per-layer host submissions.
- Restored contexts require simultaneous weight copies that push the process over
  the 11 GiB hard memory gate.
- Mutable or host-updated shared KV state is unreliable across repeated graph
  execution.
- W4A16 translation quality misses the eventual multilingual acceptance bound and
  W8A16 cannot meet memory or speed gates.
- Sustained HTP calls reproduce multi-second stalls that cannot be eliminated by
  graph fusion, partitioning, or runtime upgrade.

If W4A16 quality fails but W8A16 fits, ship W8A16 only if it still exceeds the
10-token/s minimum. If direct QNN graph construction cannot meet the dispatch or
state requirements, retain the freestanding tokenizer and artifact work but do
not hide a Genie or ONNX Runtime dependency behind the production interface.

## Validation commands

Stage 0 retains the existing commands:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\build.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\test-npu-probe.ps1
llvm-readobj --file-headers --coff-imports .\experimental\snapdragon\build\npu_probe.exe
```

Stage 4 builds and runs entirely in C after artifact export:

```powershell
.\experimental\snapdragon\tools\build-gemma.ps1 -Test
```

Regenerate the pinned tokenizer tables and reference corpus only when needed:

```powershell
.\experimental\snapdragon\tools\build-gemma.ps1 -ExportReference -Test
```

See [README.md](README.md#translategemma-development) for the optional reference
environment setup. Later stages should retain the same build/runtime separation;
the following translation interface is not implemented yet:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\build-gemma.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\experimental\snapdragon\tools\test-gemma.ps1
.\experimental\snapdragon\build\translategemma.exe --from=cs --to=de-DE < input.txt
```

## References

- [TranslateGemma collection](https://huggingface.co/collections/google/translategemma)
- [TranslateGemma 4B model card](https://huggingface.co/google/translategemma-4b-it)
- [TranslateGemma technical report](https://arxiv.org/abs/2601.09012)
- [Gemma 3 technical report](https://arxiv.org/abs/2503.19786)
- [Qualcomm AI Hub Qwen3 4B](https://aihub.qualcomm.com/models/qwen3_4b)
- [Existing Snapdragon QNN findings](snapdragon-x-qnn.md)
- [Existing Whisper benchmark record](benchmark.md)