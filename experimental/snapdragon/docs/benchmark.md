# Snapdragon Whisper benchmarks

This is the active benchmark, profiling, and optimization log for the Windows ARM64 Snapdragon path. Use fixed audio, one persistent process, exact model/cache identity, and transcript comparison when evaluating a change. Absolute CPU timings vary with Windows scheduling and power state, so retain stage timings and repeat material comparisons.

## Reference workload

- Audio: `data/bundestag-hearing-5min-16k-mono-f32.wav`.
- Format: 300.000 seconds, mono 16 kHz `pcm_f32le`, 19,200,092 bytes.
- SHA-256: `9fd103a8f8ccf691e1ce287ab76e0d2a157c621d6abc32f4d07aa984a0f9ca8c`.
- Command: `tools/profile-whisper.ps1 -Model small -WavPath <reference WAV> -OutputDirectory <result directory>`.
- Runtime shape: 12 overlapping 30-second windows at a 25-second stride in one persistent process.

## Exact decoder work reuse

The decoder now removes repeated host work without changing suppression, temperature seeds, Gumbel arithmetic, or tie-breaking:

- Token suppression and dynamic no-repeat exclusions use a per-step bitset instead of vocabulary-wide repeated linear searches.
- Retries reuse final hidden states for an unchanged token prefix. Existing CPU or HTP self-K/V entries for that prefix remain valid. The first differing token invalidates the saved suffix, and every audio window resets the prefix cache. Sampling still runs for every decoder step with the current temperature and seed.
- Gumbel samples are cached by position, token, and retry seed across windows. Storage is allocated only when temperature sampling is requested; allocation failure retains direct computation. A changed seed replaces that position's cached values. Small's complete optional table commits 92,942,080 bytes (88.64 MiB); physical residency depends on the positions used. Hidden-prefix storage adds 1,376,256 bytes (1.31 MiB), plus token metadata.
- HTP self-attention reuses the causal mask across layers at the same position, including exact FP16 zero and negative-mask values. This does not change graph shapes or the two-submission self-attention contract.

`decoder steps` still counts selection steps, including retries. `decoder prefix reused steps` counts steps that skipped transformer execution; the profiler reports `transformer_steps` as their difference. Reduced submission counts therefore do not imply fewer generated tokens. Gumbel cache reuse does not persist model activations between audio windows.

Both profiling scripts accept `-ProbePath` to compare a separately built executable while retaining the original model context and working directory. Build candidates with `tools/build.ps1 -BuildDir experimental/snapdragon/build/candidate`, stage the matching QNN runtime beside them, and pass the candidate executable to the profiler. Keep binaries unchanged throughout each comparison and retain their hashes. The production offload default remains `cross,mlp` until the relevant transcript and sustained performance gates pass.

Validation includes exhaustive fixed-suppression decisions, dynamic exclusions, prefix hit/divergence/reset behavior, exact Gumbel samples across the vocabulary, optional-cache allocation failure, and allocation cleanup. Paired 35-second Tiny, Base, and Small hardware runs in both `cross,mlp` and `fused,self,logits` retained their baseline transcript hashes.

## Baseline: CPU decoder MLP

| Metric | Result |
| --- | ---: |
| Wall time | 175.076 s |
| Throughput | 1.714x real time |
| Process CPU time | 504.672 s |
| Average CPU occupancy | 2.883 cores / 24.02% of 12-core capacity |
| Decoder | 168.175 s / 96.06% of wall |
| CPU cross-attention | 75.586 s / 43.17% of wall |
| CPU feed-forward | 54.572 s / 31.17% of wall |
| CPU self-attention | 22.785 s / 13.01% of wall |
| CPU final norm/logits | 15.200 s / 8.68% of wall |
| Existing NPU graph calls | 2.171 s / 1.24% host-observed duty |
| Peak working set | 717,520,896 bytes |
| Peak private committed | 494,166,016 bytes |

The run emitted 1,314 tokens but executed 3,841 decoder steps. Deterministic repetition retries added 2,479 steps, or 64.54% of decoder work, across nine of twelve windows.

## Experiment: per-layer decoder MLP on HTP

The first offload target is each layer's `FullyConnected -> Gelu -> FullyConnected` MLP. CPU LayerNorm and residual addition remain FP32. QNN receives one width-sized FP16 vector and returns one width-sized FP16 projection, while both matrices and biases remain static in the model-specific context. This avoids transferring self/cross-attention KV caches.

Small context construction finalized all 12 MLP graphs. Each graph reported zero VTCM spill/fill, about 9.59 MB of DDR reads, and 49 KB of writes. The serialized context grew from 210,792,448 to 325,865,472 bytes.

First-window comparison:

| Metric | CPU MLP | NPU MLP | Change |
| --- | ---: | ---: | ---: |
| Total window | 17.200 s | 13.298 s | -22.7% |
| Decoder | 16.800 s | 12.899 s | -23.2% |
| Feed-forward stage | 5.461 s CPU | 1.914 s NPU | -65.0% |
| Generated tokens / decoder steps | 107 / 447 | 107 / 447 | unchanged |
| Peak working set | 715,628,544 | 841,883,648 bytes | +17.6% |

The transcript is unchanged on this window. The memory increase comes from retaining CPU fallback weights while the serialized QNN context also embeds MLP weights. A later memory pass should either omit/decommit CPU MLP matrices after successful graph restoration or use a decoder artifact that separates fallback-only matrices.

Five-minute comparison:

| Metric | CPU MLP | NPU MLP | Change |
| --- | ---: | ---: | ---: |
| Wall time | 175.076 s | 115.821 s | -33.8% |
| Audio throughput | 1.714x | 2.590x real time | +51.1% |
| Process CPU time | 504.672 s | 379.328 s | -24.8% |
| Average occupied cores | 2.883 | 3.275 | +13.6% |
| Exact NPU host-call duty | 1.24% | 15.28% | 12.3x |
| Decoder time | 168.175 s | 109.351 s | -35.0% |
| CPU feed-forward | 54.572 s | 0.000 s | -100% |
| NPU feed-forward stage | 0.000 s | 15.986 s | new |
| NPU MLP `graphExecute` | 0.000 s | 15.553 s | new |
| Peak working set | 717,520,896 | 844,337,152 bytes | +17.7% |
| Peak private committed | 494,166,016 | 495,054,848 bytes | +0.2% |

The NPU path emitted 1,317 tokens in 3,586 steps versus 1,314 tokens in 3,841 steps for the CPU path. FP16 MLP intermediates changed several decoding decisions and reduced retries by 255 steps; the complete stitched transcript is therefore not byte-identical, although the first reference window remains byte-identical and the full output is coherent. Treat this as a reviewed model-specific quality result rather than an exact arithmetic substitution.

An earlier 12-worker run took 111.776 seconds and 370.859 CPU-seconds, showing the expected scheduling/power-state variance and a best observed 2.684x real-time throughput. An eight-worker repeat took 116.918 seconds and 374.391 CPU-seconds. Twelve workers remain the measured Small default after MLP offload.

## Utilization semantics

Windows publishes no dedicated NPU counter set for this Qualcomm driver. Generic Compute engine counters are intermittent and returned invalid samples during profiling. Report `NPU host-call duty` as the sum of synchronous QNN graph execution durations divided by process wall time. The decoder records `graph_execute` separately from FP16 conversion, LayerNorm, and residual work; the five-minute run spent 15.553 seconds in MLP execution versus 15.986 seconds in the complete offload stage. Host-call duty measures how much wall time the host waits in accelerator calls; it is not HTP arithmetic occupancy.

## Optimization work plan

Use end-to-end wall time, process CPU-seconds, exact synchronous QNN duty, retry steps, peak memory, and transcript quality as the common acceptance gates. A higher NPU percentage is not a success if total work or latency increases.

1. **First pass complete: reduce retry amplification.** Small now retries only a repeated trigram, rather than treating four ordinary repeated bigrams anywhere in a 30-second passage as a failure. Greedy decoding retains its no-repeat four-gram constraint; temperature retries use no-repeat trigrams, so one constrained retry must satisfy the Small acceptance rule. Tiny remains greedy and Base retains its established scoring behavior. If the remaining retry cost is material after further offload, investigate prefix reuse between greedy and temperature attempts.
2. **Second pass complete: keep cross-attention K/V resident on HTP.** FastRPC shared buffers now retain head-major K/V for the complete window, and per-layer QNN graphs consume registered memory handles without copying the caches per token. CPU cross-attention remains available as a fallback. The fixed Small workload reduced process CPU time by 69.3% and raised exact NPU host-call duty to 36.37%, while wall time remained effectively flat.
3. **Third pass implemented, latency gate failed: offload vocabulary projection.** A cached HTP graph now performs final LayerNorm and the complete 51,865-row vocabulary projection. CPU code retains exact suppression, no-repeat, tie-breaking, and temperature/Gumbel selection over the returned FP16 logits. QNN HTP contains a `TopK` operator, but its exact public parameter schema is unavailable; guessing that ABI is not acceptable. Pre-Gumbel TopK would also change temperature sampling semantics, so candidate reduction remains a greedy-only follow-up.
4. **Fourth pass implemented, latency gate failed: stateful HTP self-attention.** Mutable self K/V now lives in registered FastRPC shared memory; CPU writes only the current FP16 row and supplies the causal mask. Each layer uses one projection graph and one masked-attention/output graph. This removes CPU self-attention and full-cache transfers, but the two submissions per layer made the fixed Small workload slower and exposed multi-minute HTP driver stalls. Keep the CPU fallback and require a fused decoder-layer graph or another measured submission reduction before making this path a performance recommendation.
5. **Batch independent long-form windows.** Test two- and four-window graph batches to amortize dispatch and improve arithmetic intensity. Preserve output ordering and resumable window records; measure memory growth and latency as well as throughput.
6. **Enable HTP performance controls and hardware profiling.** Add the required QAIRT ABI for performance/DCVS configuration and profile events. Compare burst and sustained settings, DDR traffic, accelerator cycles, and stalls. Keep host-call duty clearly labeled as distinct from hardware occupancy.
7. **Reduce MLP weight bandwidth.** Evaluate per-channel mixed-precision weights because each Small MLP layer currently reads about 9.59 MB from DDR per step. Existing per-tensor quantization is not quality-viable, so require transcript and numerical gates for every candidate format.
8. **Apply secondary memory and segmentation improvements.** Fold LayerNorm/residual operations into accelerator graphs only where fusion reduces submissions, release duplicated CPU MLP weights after reliable QNN restore, and test smaller or VAD-selected overlap without losing boundary text.

The existing 12-worker CPU width remains the default: eight workers were slower and consumed more CPU-seconds, and idle task-pool workers already block instead of spinning.

### Next optimization priorities

Small remains the default model, but the production decoder mode returns to resident HTP cross-attention plus HTP MLP. Stateful self-attention and full-vocabulary projection remain independently selectable experiments until they improve both wall time and process CPU time. Higher host-call duty alone is not an acceptance result.

1. **Complete:** `--decoder-offload` accepts `cpu`, `all`, or comma-separated `cross`, `mlp`, `self`, `logits`, and `fused` selections. The native probe and every profile record the canonical mode. `cross,mlp` remains the default.
2. **Complete:** native profiles report offload calls, graph submissions, maximum call duration, calls over 10/100/1000 ms, complete process time, and cleanup time. Transcript SHA-256 is included in profiler output. Native and wrapper time agreed within 50 ms in the validation run; cleanup was 497 ms, so teardown does not explain the earlier anomalous wrapper sample.
3. **Implemented, latency gate not yet passed:** one 16-node graph per layer now fuses cross-attention, both residuals, MLP LayerNorm, and MLP. It halves cross/MLP submissions and reduces their HTP execution time, but the first Small five-minute run was 3.6% slower overall than separate graphs. Keep it selectable as `fused`, not as the default.
4. Build batch-2 and batch-4 variants for independent long-form windows after fusion is stable. Measure throughput, first-window latency, memory, ordering, and resumability separately.
5. Reduce greedy vocabulary output with a verified QNN `TopK` or `ArgMax` contract. CPU must validate dynamic suppression and fall back to full logits when the candidate set is exhausted. Temperature/Gumbel retries retain full-vocabulary semantics unless sampling also moves to HTP.
6. Revisit self-attention only after position-specific profiling. Test active-prefix graph buckets, direct current-row output binding, and a verified in-graph cache update. Require fewer submissions and no fixed 448-position work for short prefixes.
7. Add QNN hardware event enumeration and performance/DCVS controls, then evaluate mixed-precision decoder weights only after dispatch and graph-switching costs are under control.

For each optimization, run the focused 35-second Small gate and at least three interleaved warm repetitions of the fixed five-minute workload. Report transcript identity or reviewed quality, retry steps, native wall time, process CPU-seconds, graph submissions, per-stage median/p95/p99/maximum when available, calls over 10/100/1000 ms, peak resident/private memory, and context restore/cleanup time.

`tools/benchmark-whisper-offloads.ps1` automates the interleaved repetitions, preserves every raw profile, hashes the extracted transcript, and writes `runs.csv`, `aggregate.csv`, and `aggregate.json`. Its default matrix compares `cross,mlp`, `fused`, and `fused,self,logits` on Small.

### Retry policy result

The first implementation applied no-repeat trigrams to greedy decoding as well as retries. It was rejected immediately because it changed the Tiny regression transcript. Restricting the stronger constraint to temperature attempts preserves greedy output, and retaining the old acceptance policy for Base preserves both Tiny and Base byte-for-byte gates.

Five-minute Small result against the exact-duty MLP baseline:

| Metric | Previous policy | Aligned Small policy | Change |
| --- | ---: | ---: | ---: |
| Wall time | 115.821 s | 70.043 s | -39.5% |
| Audio throughput | 2.590x | 4.283x real time | +65.4% |
| Process CPU time | 379.328 s | 225.344 s | -40.6% |
| Decoder time | 109.351 s | 63.735 s | -41.7% |
| Decoder steps | 3,586 | 2,190 | -38.9% |
| Retry steps | 2,221 | 758 | -65.9% |
| CPU cross-attention | 61.380 s | 37.198 s | -39.4% |
| CPU self-attention | 19.647 s | 11.777 s | -40.1% |
| CPU final norm/logits | 12.312 s | 5.134 s | -58.3% |
| NPU MLP `graphExecute` | 15.553 s | 9.350 s | -39.9% |
| Exact NPU host-call duty | 15.28% | 16.41% | +1.13 points |

Nine of twelve stitched window updates remained unchanged. In particular, the former 447- and 622-step windows now produce the same text in 111 and 157 steps. Three windows selected different bounded candidates: the largest change replaces a malformed 26-token agenda fragment with a coherent 108-token passage, while the other two preserve the surrounding subject and remove duplicated overlap. The complete output remains coherent through the five-minute endpoint. Generated tokens increased from 1,317 to 1,384 because the repaired agenda window is no longer prematurely short.

### Resident cross-attention K/V result

The decoder now allocates one contiguous K/V store with FastRPC `rpcmem_alloc`, registers it with QNN as an HTP shared buffer, and binds per-layer offsets through graph I/O memory handles. The once-per-window producer graph writes K/V directly in the head-major layouts consumed by attention: K as `[heads, head_width, frames]` and V as `[heads, frames, head_width]`. Each decoder layer has a cached HTP graph for LayerNorm, pre-scaled Q projection, both attention matrix multiplications, softmax, and output projection. Only the width-sized residual state and projected result cross the graph boundary per token; the complete K/V cache is never copied per token.

Fresh Tiny, Base, and Small contexts built and restored successfully on HTP. Focused Tiny and Base runs reported zero CPU cross-attention and retained their expected transcripts. On the five-minute Small workload, all 12 stitched transcript updates are byte-identical to the accepted retry-policy baseline.

Five-minute comparison:

| Metric | CPU cross-attention | Resident HTP K/V | Change |
| --- | ---: | ---: | ---: |
| Wall time | 70.043 s | 70.651 s | +0.9% |
| Audio throughput | 4.283x | 4.246x real time | -0.9% |
| Process CPU time | 225.344 s | 69.094 s | -69.3% |
| Average occupied cores | 3.217 | 0.978 | -69.6% |
| Decoder time | 63.735 s | 62.590 s | -1.8% |
| CPU cross-attention | 37.198 s | 0.000 s | -100% |
| NPU cross-attention stage | 0.000 s | 12.158 s | new |
| NPU cross-attention `graphExecute` | 0.000 s | 11.951 s | new |
| NPU MLP `graphExecute` | 9.350 s | 11.502 s | +23.0% |
| Exact NPU host-call duty | 16.41% | 36.37% | +19.96 points |
| Generated tokens / decoder steps | 1,384 / 2,190 | 1,384 / 2,189 | unchanged |
| Retry steps | 758 | 757 | unchanged |
| Peak working set | 844,316,672 | 971,997,184 bytes | +15.1% |
| Peak private committed | 495,857,664 | 496,955,392 bytes | +0.2% |

Cross-attention itself is substantially cheaper on HTP and removes the dominant CPU stage, but this sustained run did not improve wall throughput. CPU self-attention rose from 11.777 to 21.638 seconds, final norm/logits from 5.134 to 16.731 seconds, and MLP graph execution from 9.350 to 11.502 seconds. The process nevertheless consumed 156.25 fewer CPU-seconds. This makes the pass a strong CPU-use and NPU-duty improvement with neutral latency; the next vocabulary-projection and stateful self-attention passes should address the CPU frequency/scheduling-sensitive stages that now dominate wall time.

### Final projection and stateful self-attention result

The final projection graph accepts the FP32 decoder state, applies final LayerNorm and the tied FP16 token embedding matrix on HTP, and returns all 51,865 FP16 logits. Returning the full vocabulary preserves exact CPU suppression and sampling, but CPU final-norm/logit accounting still includes conversion and a full vocabulary scan. On the focused Small window the graph executed in 235.156 ms while CPU candidate processing took 308.835 ms.

Self-attention uses one registered allocation for immutable cross K/V and mutable self K/V. For each layer and token, an HTP projection graph emits Q/K/V, CPU copies only the current K/V row into its head-major cache, and a second HTP graph performs masked attention and output projection. The accepted 107-token focused transcript is unchanged; all CPU self-attention, cross-attention, and feed-forward counters are zero. That run took 3.263 seconds in the decoder, including 1.165 seconds of self-attention `graphExecute`, 654.316 ms of cross-attention, 754.143 ms of MLP, and 235.156 ms of final projection. Peak resident memory was 1,255,768,064 bytes.

The fixed five-minute run preserved transcript quality and produced 1,383 tokens in 2,191 steps with 760 retry steps. It failed the latency gate:

| Metric | Resident cross K/V | Stateful self K/V + final projection | Change |
| --- | ---: | ---: | ---: |
| Instrumented window time | 70.651 s | 422.627 s | +498.2% |
| Process CPU time | 69.094 s | 74.922 s | +8.4% |
| CPU self-attention | 21.638 s | 0.000 s | -100% |
| CPU cross-attention | 0.000 s | 0.000 s | unchanged |
| CPU feed-forward | 0.000 s | 0.000 s | unchanged |
| CPU final norm/logits | 16.731 s | 23.864 s | +42.6% |
| NPU self-attention `graphExecute` | 0.000 s | 52.981 s | new |
| NPU cross-attention `graphExecute` | 11.951 s | 300.148 s | driver stall |
| NPU MLP `graphExecute` | 11.502 s | 28.507 s | +147.8% |
| NPU final projection `graphExecute` | 0.000 s | 6.421 s | new |
| Peak resident bytes | 971,997,184 | 1,255,710,720 | +29.2% |

One 122-step window accumulated 273.400 seconds in cross-attention `graphExecute`; a separate run stalled in self-attention instead. Excluding that outlier window, the other eleven windows still totaled about 140 seconds, roughly twice the resident-cross baseline. The PowerShell wrapper reported an inconsistent 4,116-second wall interval, so the table uses native per-window timers and does not derive NPU duty from the wrapper value. The implementation proves state residency and exact fallback behavior, but not a deployable speedup.

### Fused cross-attention and MLP result

The fused graph consumes the FP16 residual state and resident cross K/V, then performs cross LayerNorm, attention, cross residual, final LayerNorm, both MLP projections, GELU, and the final residual in one `graphExecute`. CPU fallback and the separate cross/MLP graphs remain available for controlled comparisons. Moving residuals inside the FP16 graph preserved the focused Tiny, Base, and Small transcripts.

On Tiny, fusion reduced decoder time from 599.011 to 362.847 ms, reduced cross/MLP graph execution from 252.655 to 127.399 ms, and cut submissions from 848 to 424. On the focused Small gate it reduced decoder time from 2.770 to 2.560 seconds and submissions from 2,664 to 1,332; twelve first-use calls exceeded 10 ms and the maximum was 55.983 ms.

The first fixed five-minute Small comparison used the same expanded context for all modes:

| Metric | Separate `cross,mlp` | `fused` | `fused,self,logits` |
| --- | ---: | ---: | ---: |
| Wall time | 76.684 s | 79.441 s | 77.845 s |
| Native process time | 76.636 s | 79.383 s | 77.783 s |
| Instrumented window time | 72.298 s | 75.941 s | 73.834 s |
| Process CPU time | 78.672 s | 93.484 s | 67.641 s |
| NPU host-call duty | 33.43% | 24.88% | 61.22% |
| Cross + MLP/fused `graphExecute` | 23.409 s | 17.407 s | 18.533 s |
| Self-attention `graphExecute` | 0.000 s | 0.000 s | 22.183 s |
| Final projection `graphExecute` | 0.000 s | 0.000 s | 4.717 s |
| Decoder graph submissions | 52,536 | 26,268 | 81,030 |
| Maximum offload call | 19.812 ms | 29.797 ms | 16.847 ms |
| Calls over 100 ms | 0 | 0 | 0 |
| Tokens / steps / retries | 1,384 / 2,189 / 757 | 1,384 / 2,189 / 757 | 1,382 / 2,190 / 760 |
| Peak resident bytes | 1,629,048,832 | 1,629,048,832 | 1,629,036,544 |

Fusion succeeds at its direct objective: it halves submissions and cuts cross/MLP HTP execution by 25.6%. It does not yet improve end-to-end Small latency because CPU self-attention and logits become slower under the changed scheduling/power profile. Adding self-attention and logits reaches 61.2% host-call duty and reduces CPU time by 14.0% versus the same-context separate mode, but remains 1.5% slower and changes two generated tokens. The production default therefore remains `cross,mlp` pending repeated interleaved measurements and the next batching/candidate-reduction work.