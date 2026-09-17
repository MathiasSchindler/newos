# Whisper multi-model refactor tracker

This is a temporary implementation tracker. Delete it when the refactor is complete. Durable runtime behavior and contributor guidance belong in `README.md` or `npu-plan.md`.

## Goals

- Preserve the validated Whisper Tiny transcript and performance baseline.
- Add Whisper Base first, then make Whisper Small practical.
- Keep the runtime freestanding C with no CRT or libc.
- Keep QNN graphs and context binaries fixed-shape and model-specific.
- Make CPU decoder kernels, allocation, orchestration, and artifact validation model-aware.
- Preserve focused profiling so future optimization work remains measurable.

## Non-goals

- A generic neural-network graph runtime.
- Runtime mutation of fixed QNN graph shapes.
- Supporting every Whisper checkpoint in the first pass.
- Loading unvalidated model metadata or arbitrary tensor layouts.

## Model targets

| Model | Width | FFN width | Heads | Encoder layers | Decoder layers | Vocabulary | Audio context |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Tiny | 384 | 1536 | 6 | 4 | 4 | 51865 | 1500 frames |
| Base | 512 | 2048 | 8 | 6 | 6 | 51865 | 1500 frames |
| Small | 768 | 3072 | 12 | 12 | 12 | 51865 | 1500 frames |

All three use a head width of 64, a 448-token text context, and the current 80-bin audio frontend. Later variants with a different mel-bin count require an explicit frontend contract change.

## Invariants

- Artifact headers carry a format version and complete model dimensions.
- Runtime dimensions are validated against bounded capacities before allocation or pointer binding.
- Integer byte-count calculations are overflow-checked.
- QNN context caches are keyed by model identity and graph contract; caches are never shared across incompatible models.
- Per-model graph topology remains generated and fixed-shape.
- Optimized kernels receive dimensions through a runtime context and retain specialized fast paths where measurements justify them.
- Tiny output remains the regression oracle until Base has independent reference fixtures.

## Implementation phases

### Phase 0: Baseline and tracker

- [x] Record the current architecture and constraints in this tracker.
- [x] Keep the existing Tiny build, mock suite, PE audit, and 35-second HTP transcript as gates.
- [x] Add a compact checked-size helper for model-dependent allocation calculations.

Exit criteria: the baseline commands and expected artifacts are explicit, and no behavior changes.

### Phase 1: Canonical model descriptor

- [x] Add `WhisperModelConfig` with model identity, width, FFN width, heads, encoder/decoder layers, vocabulary size, text context, mel bins, and encoder frames.
- [x] Define the validated Tiny configuration once and consume its canonical constants from frontend, decoder, QNN bridge, and probe code.
- [x] Add validation for structural invariants such as `width % heads == 0` and bounded layer/output counts.
- [ ] Remove duplicate Tiny architecture literals from production paths.
- [x] Keep compile-time constants only where C array layout or a measured specialized kernel requires them temporarily.

Exit criteria: Tiny builds and produces the same transcript; no production module independently defines Tiny architecture.

### Phase 2: Versioned artifact contracts

- [x] Define a shared versioned artifact header containing model identity, dimensions, payload type, element type, element count, and integrity metadata.
- [x] Update decoder weights, token bytes, cross-K/V weights, frontend bundles, and encoder bundles.
- [x] Regenerate all artifacts atomically rather than retaining a version-1 Tiny reader.
- [x] Reject mismatched dimensions before allocating payload storage or binding tensors.
- [x] Generalize the exporter to read a pinned model catalog plus checkpoint `config.json` instead of Tiny constants.

Exit criteria: corrupted, truncated, wrong-model, and wrong-shape artifacts fail before inference; regenerated Tiny output remains unchanged.

### Phase 3: Runtime storage and decoder context

- [x] Introduce a `WhisperDecoder` context instead of decoder-global model state.
- [x] Allocate weights, self-attention caches, cross-attention fallback storage, scratch vectors, scores, and layer tables with checked `VirtualAlloc` arenas.
- [x] Parameterize weight binding and decoder loops by the validated model descriptor.
- [x] Preserve 16-byte or stronger alignment required by ARM64 vector kernels.
- [x] Keep vectorized kernels runtime-sized; no unbenchmarked dimension-specialized dispatch is retained.
- [x] Route every partial-allocation failure through one cleanup path and make null shutdown safe.

Exit criteria: Tiny uses runtime-sized storage, passes failure cleanup tests, and has no material decoder regression.

### Phase 4: QNN cache and graph separation

- [x] Replace the fixed eight-output cross-K/V contract with a bounded descriptor-driven output table.
- [x] Generate tensor and node names by layer rather than storing Tiny-only name arrays.
- [x] Include model identity and all tensor IDs in the context-cache header.
- [x] Use distinct context-cache files and graph names per model.
- [x] Split cache construction and diagnostic probes from the production transcription orchestration so runtime builds do not reserve builder-only model buffers.

Exit criteria: Tiny cache rebuild/restore works; a wrong-model cache is rejected; production transcription does not depend on diagnostic fixture storage.

### Phase 5: Whisper Base vertical slice

- [x] Add a pinned Base checkpoint fetch definition and hashes.
- [x] Export Base decoder, tokenizer, frontend, encoder, and cross-K/V artifacts.
- [x] Build and restore a Base-specific QNN context cache.
- [x] Add `--model=base` while retaining Tiny as the default.
- [x] Validate one 30-second reference window, the 35-second overlap fixture, UTF-8 output, quiet mode, and cleanup failures.
- [x] Record memory, per-stage latency, tokens per second, and transcript quality against Tiny.

Exit criteria: Base completes native long-form transcription without code copied from the Tiny runtime.

### Phase 6: Weight traffic and Whisper Small

- [x] Implement and validate FP16 decoder-weight storage with FP32 accumulation, beginning with embeddings and MLP matrices.
- [x] Benchmark kernel tiling and worker partitioning separately for widths 384, 512, and 768.
- [x] Measure resident/committed memory rather than relying only on artifact size.
- [x] Add the pinned Small checkpoint only after the Base path and FP16 weight path are stable.
- [x] Reconsider additional QNN decoder offload only if persistent device memory/state becomes available.

Exit criteria: Small runs within an explicit memory budget and its speed/quality tradeoff is documented.

## Validation matrix

For every phase that changes runtime code:

- `experimental/snapdragon/tools/whisper/build.ps1`
- `experimental/snapdragon/tools/whisper/test-npu-probe.ps1`
- Native 35-second, two-window HTP transcription in default and `--quiet` modes
- Transcript equality or an explicitly reviewed model-specific reference
- ARM64 PE inspection: no exception table and only expected static imports
- `git diff --check`

For artifact-format changes, additionally test bad magic, unsupported version, dimension mismatch, truncated payload, overflow-sized counts, and model/cache mismatch.

## Progress log

- 2026-09-12: Started tracker after the Tiny end-to-end, persistent long-form, incremental output, and quiet-mode paths were validated. Selected a descriptor-first refactor with Base as the second model and model-specific QNN caches.
- 2026-09-12: Added `whisper_model.h/.c`, canonicalized Tiny dimensions used by the frontend, decoder, QNN cross-K/V bridge, and probe, and added bounded descriptor validation plus checked artifact-count arithmetic. The freestanding build and all 12 mock lifecycle cases pass; the real two-window HTP transcript remains byte-identical to the 538-byte pre-refactor quiet baseline.
- 2026-09-12: Completed Phase 2 with a shared 96-byte little-endian version-2 header, atomic catalog-driven export, model-aware decoder/cross-K/V/QNN context readers, regenerated Tiny production artifacts, and 18 no-CRT format checks covering magic, version, dimensions, truncation, hashes, overflow fields, and cache model identity. Both fresh and restored QNN context paths preserve the 538-byte transcript.
- 2026-09-12: Completed Phase 3 by moving all decoder state into a `WhisperDecoder`, replacing Tiny-sized globals with checked runtime arenas, and driving weight binding and generation from the descriptor. All four owned allocation failures and normal shutdown return the observed allocation count to zero; 12 QNN mock cases pass, default output retains two incremental updates, quiet output is byte-identical, and measured decoder time remains within ordinary run variance.
- 2026-09-12: Completed Phase 4 with model-derived QNN graph/tensor names, bounded decoder output IDs, model-specific context artifacts, an explicit `WhisperDecoderQnn`, and separate 33,280-byte runtime and 90,112-byte cache-builder binaries. Tiny context build/restore works on HTP, wrong-model metadata is rejected, and the 538-byte quiet transcript remains byte-identical.
- 2026-09-12: Completed Phase 5 with the pinned multilingual Base checkpoint and complete version-2 decoder, token, cross-K/V, frontend, and six-layer encoder bundles. The 49,713,552-byte Base context builds, restores, and executes on HTP; a 30-second window completed in 2.133 seconds with 114 generated tokens, and the 35-second quiet path emitted 539 bytes of strict UTF-8 with empty stderr. Peak working set was 362,921,984 bytes versus Tiny's 217,350,144 bytes. Tiny still produced 92 tokens in 1.073 seconds on the matched window, but its transcript repeated and lost substantially more content than Base.
- 2026-09-12: Added a Base-only deterministic temperature fallback for greedy results with excessive repeated token bigrams and trigrams. The previously collapsed 300-second hearing window now emits a coherent 475-byte passage; reruns are byte-identical, while the established 539-byte Base and 538-byte Tiny fixtures remain unchanged.
- 2026-09-12: Completed Phase 6 with homogeneous FP16 decoder bundles and FP32 activation/accumulation. Tiny and Base decoder storage fell from 118,212,192 to 59,106,144 bytes and from 208,017,504 to 104,008,800 bytes while preserving their established transcripts. A freestanding ARM64 benchmark now sweeps 1/2/4/8 accumulator kernels and 1/4/8/12 worker partitions at widths 384, 512, and 768; repeated checksum-identical runs showed dimension and power-state sensitivity, so the stable four-accumulator production kernel and explicit worker override remain preferable to an unverified universal specialization.
- 2026-09-12: Added pinned multilingual Small with width 768 and 12 encoder/decoder layers. Its 307,164,768-byte FP16 decoder, 28,333,152-byte cross-K/V bundle, and 210,792,448-byte serialized QNN context restore and execute successfully. The reference window produced a coherent 107-token German transcript in 17.200 seconds total. Peak working set was 715,628,544 bytes and private committed memory was 494,174,208 bytes, satisfying the explicit limits of 768 MiB resident and 512 MiB private. CPU cross-attention and feed-forward remain dominant; per-token QNN decoder offload is still rejected until persistent device-resident mutable state is available.
