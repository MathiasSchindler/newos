# Snapdragon Whisper benchmarks

This is the active benchmark, profiling, and optimization log for the Windows ARM64 Snapdragon path. Use fixed audio, one persistent process, exact model/cache identity, and transcript comparison when evaluating a change. Absolute CPU timings vary with Windows scheduling and power state, so retain stage timings and repeat material comparisons.

## Reference workload

- Audio: `data/bundestag-hearing-5min-16k-mono-f32.wav`.
- Format: 300.000 seconds, mono 16 kHz `pcm_f32le`, 19,200,092 bytes.
- SHA-256: `9fd103a8f8ccf691e1ce287ab76e0d2a157c621d6abc32f4d07aa984a0f9ca8c`.
- Command: `tools/profile-whisper.ps1 -Model small -WavPath <reference WAV> -OutputDirectory <result directory>`.
- Runtime shape: 12 overlapping 30-second windows at a 25-second stride in one persistent process.

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
3. **Offload vocabulary projection and candidate selection.** Prototype a cached final LayerNorm and vocabulary `FullyConnected` graph. Prefer device-side `TopK` so CPU suppression and temperature logic receive only a small candidate set. The current CPU final-norm/logit opportunity is 12.31 seconds.
4. **Move self-attention to stateful HTP graphs.** Keep mutable self K/V on device and update only the current position. Once state handling is proven, fuse self-attention, cross-attention, and MLP into one graph per decoder layer. The current self-attention opportunity is 19.65 seconds.
5. **Batch independent long-form windows.** Test two- and four-window graph batches to amortize dispatch and improve arithmetic intensity. Preserve output ordering and resumable window records; measure memory growth and latency as well as throughput.
6. **Enable HTP performance controls and hardware profiling.** Add the required QAIRT ABI for performance/DCVS configuration and profile events. Compare burst and sustained settings, DDR traffic, accelerator cycles, and stalls. Keep host-call duty clearly labeled as distinct from hardware occupancy.
7. **Reduce MLP weight bandwidth.** Evaluate per-channel mixed-precision weights because each Small MLP layer currently reads about 9.59 MB from DDR per step. Existing per-tensor quantization is not quality-viable, so require transcript and numerical gates for every candidate format.
8. **Apply secondary memory and segmentation improvements.** Fold LayerNorm/residual operations into accelerator graphs only where fusion reduces submissions, release duplicated CPU MLP weights after reliable QNN restore, and test smaller or VAD-selected overlap without losing boundary text.

The existing 12-worker CPU width remains the default: eight workers were slower and consumed more CPU-seconds, and idle task-pool workers already block instead of spinning.

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