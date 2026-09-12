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

## Optimization order

1. Reduce retry amplification or reuse work between deterministic retries.
2. Prototype cross-attention on HTP only with persistent device-resident K/V memory; repeated host transfer of the complete cache is not acceptable.
3. Consider vocabulary projection offload as one graph per token if measured dispatch plus FP16 transfer beats the current CPU scan.
4. Reclaim duplicated CPU/QNN MLP weight residency after the offload path is proven reliable.