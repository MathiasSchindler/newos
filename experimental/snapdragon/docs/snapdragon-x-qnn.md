# Snapdragon X QNN notes

These are reusable findings from running freestanding ARM64 Whisper inference through QAIRT 2.42 / QNN core 2.32 on Windows on Snapdragon X.

## Deployment boundary

- Keep cache construction and production restoration in separate executables. Graph creation/finalization needs temporary weights and diagnostics that should not enlarge the steady-state runtime.
- Treat each serialized QNN context as model- and graph-contract-specific. Store model dimensions, tensor IDs, payload size, and a hash outside the opaque QNN payload and reject mismatches before restoration.
- Stage Qualcomm's required HTP DLLs beside the executable, but resolve `QnnHtp.dll` dynamically. The freestanding executable can retain Kernel32 as its only static DLL dependency.
- Context restoration is not graph preparation. A restored context retrieves finalized graphs by name and avoids rebuilding nodes, but still incurs meaningful file I/O, backend setup, and QNN deserialization time.

## HTP behavior

- Large fixed-shape graphs can compile successfully while exceeding efficient VTCM grouping. QNN spill/fill reports are performance evidence, not correctness failures; Small's 768-wide 12-layer encoder reported roughly 357 MB spilled and 385 MB filled.
- Messages such as `UNSUPPORTED_KEY: 49/50`, uninitialized CFB callback lists, and VTCM spill warnings occurred on successful executions. Gate on QNN return codes and output validation rather than treating every backend log line as fatal.
- Cache build can take much longer and consume much more temporary memory than restore. Build once with the exact deployed runtime and retain the validated context artifact.
- Rebuild caches after changing model dimensions, tensor IDs, graph names, QNN ABI/runtime version, or graph topology. Do not infer compatibility from a successful file read.

## Decoder placement

- FP16 immutable decoder storage with FP32 accumulation halves weight traffic and avoids an FP32 shadow copy while preserving the validated Tiny and Base transcripts.
- Per-token decoder offload is unattractive without persistent device-resident mutable KV state. Re-uploading self/cross K/V tensors and dispatching multiple tiny graphs each token can cost more than the CPU matrix-vector work.
- Benchmark accumulator tiling separately from task partitioning. On Snapdragon X, width and CPU power state changed the ranking; isolated four-worker kernels did not predict Small's best complete-decoder worker count.
- Preserve an explicit worker-count override for measurements. Report repeated/interleaved samples because Windows scheduling and power state materially affect CPU decoder latency.

## Windows operations

- For large Hugging Face LFS checkpoints, use `curl.exe` retries with `--continue-at -`, download to a `.partial` path, verify exact size and SHA-256, then atomically rename. `Invoke-WebRequest` proved fragile for a roughly 967 MB checkpoint.
- Hugging Face's LFS `X-Linked-ETag` provides an independent source for the object SHA-256, but the pinned local catalog remains the deployment authority.
- Measure memory instead of estimating it from artifacts. `GetCurrentProcess` and `K32GetProcessMemoryInfo` are available through Kernel32 and expose peak/current working set plus private committed bytes without adding a CRT or another static DLL import.
- Keep generated model bundles, QNN contexts, proprietary runtimes, logs, and benchmark CSV files outside source control. Commit only reproducible exporters, pinned metadata, runtime readers, and durable conclusions.