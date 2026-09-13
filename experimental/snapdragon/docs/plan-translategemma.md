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
backbone contains about 4.30 billion BF16 parameters and uses:

| Property | TranslateGemma 4B text backbone |
| --- | ---: |
| Decoder layers | 26 |
| Hidden width | 2304 |
| MLP width | 9216 |
| Query heads | 8 |
| KV heads | 4 |
| Head width | 256 |
| Vocabulary | 262208 |
| Translation context | 2048 tokens |
| Attention pattern | Five local layers per global layer |
| Local window | 4096 architecturally, capped by the 2048 deployment context |

BF16 text weights require roughly 8.0 GiB before QNN context and runtime
overhead. That is not a practical deployment format on the target 16 GiB
machine. The production target is symmetric weight-only INT4 with FP16
activations (`W4A16`); INT8 is a bring-up and quality-control format.

At the 2048-token deployment context, an FP16 KV cache requires about 208 MiB.
The trained 4096-token local window is larger than this deployment context, so
the local/global pattern does not reduce that initial budget. KV storage is still
not the primary memory risk; duplicated weights in prompt and token-generator
contexts are.

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

QAIRT 2.50/QNN core 2.39 is now the single validated working runtime. Qualcomm's
comparable 4B packages require QNN SDK 2.45 or newer, so this satisfies the SDK
floor without keeping a parallel legacy installation.

Migration status (2026-09-13): the official archive is hash-pinned, the narrow
ABI and provider gate require QNN core 2.39, the ARM64 HTP runtime is staged from
that archive, all Whisper contexts were regenerated, and the Small transcript
gate passed. Gemma-specific capability probes remain.

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

## Stage 2: Pinned model descriptor and acquisition

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

## Stage 3: Versioned artifacts and W4A16 conversion

Create a Gemma-specific artifact contract rather than extending Whisper headers
with unrelated fields.

Required artifacts:

- W4 language-model weights and per-group scales.
- FP16 normalization and other small parameters where quantization is not useful.
- Tokenizer vocabulary, normalization data, byte mappings, and lookup tables.
- Layer-type and RoPE tables.
- Deterministic prompt, layer-output, logits, and generated-token fixtures.
- Prompt-processor and token-generator QNN context binaries.

Actions:

1. Export from the original BF16 checkpoint, not a third-party GGUF conversion.
2. Start with W8A16 as a numerical bring-up control, then implement symmetric
   W4A16 using the exact grouping and packing accepted by HTP.
3. Use multilingual translation calibration and validation data. Weight-only
   quantization should not require activation calibration, but clipping and group
   size still require measured quality gates.
4. Use a little-endian versioned header with model identity, source revision,
   tensor name or stable ID, shape, layout, element and quantization types, group
   size, payload size, and a strong payload hash.
5. Publish complete artifact sets atomically through a manifest written last.

Exit criteria:

- Every tensor round-trips through the artifact reader and agrees with the
  exporter reference.
- Wrong model, version, dimensions, layout, quantization, truncation, overflow,
  and payload corruption are rejected before QNN binding.
- W4 weight storage is in the expected 2.4-2.8 GiB range for the text model.

## Stage 4: Freestanding tokenizer and prompt contract

Implement only the tokenizer and template behavior required for TranslateGemma.

Actions:

1. Convert the Gemma SentencePiece model into a bounded binary representation
   suitable for direct C lookup. Prefer a generated trie plus explicit scores and
   byte-fallback metadata over parsing protobuf at runtime.
2. Implement UTF-8 validation, normalization required by the pinned tokenizer,
   unigram segmentation, byte fallback, special tokens, and detokenization.
3. Implement the fixed TranslateGemma text prompt directly in C from validated
   source and target language identifiers. Do not embed a Jinja interpreter.
4. Reject unsupported language identifiers and inputs that would exceed the
   2048-token deployment contract.
5. Compare token IDs and decoded bytes against the pinned Transformers tokenizer
   for ASCII, multilingual scripts, combining characters, malformed UTF-8,
   punctuation, and byte-fallback cases.

Exit criteria:

- Token IDs are identical to the reference corpus.
- Detokenized bytes are identical and valid UTF-8.
- Tokenization has deterministic allocation bounds and no CRT imports.

## Stage 5: Minimal numerical reference

Build a development-time reference that isolates model correctness from QNN
integration. It may use Python/Transformers, but all fixtures consumed by the C
runtime are immutable and hashed.

Actions:

1. Record BF16 outputs for embedding, RMSNorm, RoPE, grouped-query attention,
   gated GELU MLP, one complete local layer, one complete global layer, final
   normalization, and vocabulary projection.
2. Record W8A16 and W4A16 simulated outputs using the exact deployment packing and
   rounding rules.
3. Record greedy token sequences for a small multilingual translation corpus.
4. Add freestanding scalar checks for packing, dequantization, RoPE, mask creation,
   KV indexing, and argmax tie-breaking. Do not implement a production-speed CPU
   copy of the complete model.

Exit criteria:

- Primitive and layer fixtures identify whether differences originate in export,
  quantization, graph composition, or generation policy.
- Greedy BF16 output is reproducible for every acceptance sentence.

## Stage 6: QNN transformer block

Prove one local and one global decoder block before constructing all 26 layers.

Actions:

1. Build W8A16 graphs first and compare all meaningful intermediate outputs.
2. Replace projections with W4A16 while retaining FP16 residual streams,
   normalization, attention, and KV storage.
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

## Stage 7: Prompt processor

Create a fixed-shape prompt path that amortizes weight traffic across many input
tokens.

Actions:

1. Begin with 128-token chunks and context buckets of 512, 1024, and 2048 tokens.
2. Compose all 26 layers into one graph where QNN finalization permits it. If the
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

1. Build a one-token graph containing all 26 layers, final RMSNorm, and the tied
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

Later stages should add focused Gemma commands with the same separation:

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