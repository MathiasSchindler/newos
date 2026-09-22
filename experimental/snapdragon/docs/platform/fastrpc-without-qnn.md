# FastRPC without QNN on this Surface

Investigation date: 2026-09-22. Status: **QNN-free scalar DSP and FP16 HMX matrix
execution verified on this Surface**, including repeated numerical checks,
cleanup and negative catalog/module controls. Production inference is unchanged.

## Conclusion

There is a concrete QNN-free host interface on this machine. The installed
Qualcomm NPU/MCDM FastRPC library exposes remote open/invoke/close functions,
reports unsigned DSP protection-domain support, and allocates usable shared
memory without calling QNN. We now have an independently built LLVM 23.1.2
Hexagon compiler and a freestanding V73 scalar module. Unsigned-session
configuration, module open, scalar invocation and handle close all succeed
with the development-signed catalog and the user's configured test trust.

A separate SDK-compiled HMX variant now executes 32x32 FP16 matrix products
using V73 matrix instructions, not a scalar/HVX fallback. Three isolated signed
sessions initially passed six changing-input integer matrices: **18,432 output
elements checked exactly**, with successful resource and power cleanup. A later
signed run added fractional FP16 fixtures and a checked Whisper Tiny Q-weight
tile (see the updated receipt below). The host remains
Kernel32-only; this DSP variant uses SDK firmware ABI headers at build time and
11 weak firmware imports, but no QNN or linked C runtime/inference libraries.

The Windows Hexagon backend documentation linked below requires a trusted
signed catalog for custom DSP libraries. Unsigned DSP protection domains do
not remove that separate Windows requirement. Its documented development
procedure uses test-signing, a test certificate, and potentially disabling
Secure Boot. The user enabled test-signing, rebooted and imported the public
development certificate into Local Machine Root and TrustedPublisher. The same
module that previously failed to load now executes successfully. Removing its
adjacent catalog or changing the module bytes makes loading fail. No driver
package, Inf2Cat/SignTool installation or Microsoft signing submission was needed
for this tested development route.

Whisper, GLM-OCR and TranslateGemma remain on their working QNN paths. No
application source, installed executable, model, context, distribution, driver
or firmware was changed for this investigation. The boot-policy change above
and certificate trust imports were performed explicitly by the user; Memory
Integrity/HVCI remains enabled. The OEM driver, firmware and FastRPC library
are still required; this is QNN-free, not vendor-independent.

## Ways to use the NPU without QNN

**Recommended experimental route: keep the OEM transport and firmware, but
replace the QNN graph/runtime layer with our own FastRPC DSP module and HMX
kernels.** This is the route verified here. It does not require replacing the
Windows driver or modifying the working inference applications.

Distinguish three different goals: not calling the QNN API, not loading a QNN
runtime anywhere in the execution path, and removing all Qualcomm software.
The probe achieves the first two, not the third. Likewise, executing scalar
code on the Hexagon DSP is not proof of using its HMX matrix accelerator.

| Route | What it provides | Evidence and suitability here |
| --- | --- | --- |
| Own FastRPC module with HMX instructions | Direct matrix acceleration; we own packing, kernels and scheduling | **Verified:** integer and fractional FP16 32x32 products, one real Tiny weight tile, cleanup and negative controls. Not yet a resident model engine. |
| Own FastRPC module with scalar DSP or HVX code | DSP control, vector operations and supporting kernels | Scalar execution verified. HVX capabilities reported, but a custom HVX kernel was not tested in this investigation. These are complementary to HMX, not substitutes for matrix-execution evidence. |
| Third-party direct-Hexagon engine | An existing model runtime built over FastRPC and DSP kernels | Public implementations informed the ABI/ISA investigation; none was installed or validated as a model engine here. This introduces external implementation/runtime dependencies and is not the chosen project route. |
| Windows ML or another execution-provider wrapper | Higher-level model execution and provider management | Not demonstrated to be QNN-free on this machine. Avoiding QNN calls in application code does not establish that the selected provider avoids QNN internally. Inspect the actual provider and loaded runtime. |
| DirectML / D3D12 compute | A different Windows compute API | Not a verified path to this device's HMX NPU. Successful GPU compute or adapter enumeration must not be reported as NPU execution. |
| Direct OEM driver submissions without `libcdsprpc.dll` | Potential replacement for the vendor host transport | Not implemented. This requires a separate investigation of the Windows driver ABI, memory mapping, synchronization and trust checks. Calling `D3DKMT*` alone is not a usable NPU programming interface. |
| Linux FastRPC | Similar DSP offload concepts through Linux kernel interfaces | Useful architectural reference, not a drop-in Windows implementation or a tested alternative configuration for this Surface. |

There is no demonstrated generic Windows API that accepts arbitrary C matrix
code and automatically runs it on this NPU. The verified route builds a
separate Hexagon ELF, loads it through FastRPC and explicitly issues HMX
instructions inside it. Existing QNN context binaries cannot simply be passed
to the custom module: their graph execution and artifact formats belong to QNN.

## Programming the verified path

The two sides have different machine-code and calling conventions:

```text
our Windows ARM64 host (PE, Kernel32-only static imports)
  -> OEM MCDM libcdsprpc.dll, loaded explicitly
  -> installed Windows NPU driver and Hexagon firmware
  -> our development-catalog-signed V73 module (ELF32)
       -> firmware resource/power services
       -> scalar packing + HMX matrix instructions + result readback
```

### Host and DSP responsibilities

1. **Build the two targets independently.** The host uses Windows Clang with
   `-ffreestanding -fno-builtin -nostdlib`. The scalar DSP build uses the local
   Hexagon-enabled LLVM; the HMX variant requires the SDK compiler and
   `-mv73 -mhmx`. Compilation for Hexagon alone does not imply HMX support.
2. **Load the correct OEM transport.** Use the explicit absolute path to the
   MCDM `libcdsprpc.dll` in DriverStore. The similarly named ADSP copy did not
   provide usable cDSP capability access in this experiment. Query capabilities
   for diagnostics, but do not interpret zero HMX fields as proof of absence.
3. **Open an isolated custom-module session.** The host resolves
   `remote_session_control` and `remote_handle64_open/invoke/close`, requests
   unsigned cDSP domain 3, then opens the fixed module URI. Keep the matching
   signed catalog next to the module and set a per-child module search path.
   An unsigned protection domain does not waive Windows catalog trust.
4. **Exchange a deliberately small protocol.** Allocate shared host memory
   with `rpcmem_alloc`, pass buffers through `remote_handle64_invoke`, and
   validate their sizes/alignment on the DSP. Host `remote_arg` is 16 bytes;
   DSP `remote_arg` is 8 bytes. The current method 2 is scalar; method 3 is the
   fixed-size FP16 matrix probe. These are probe contracts, not a general
   inference API. Check both transport status and the in-band result status.
5. **Acquire accelerator resources before instructions.** The DSP requests
   VTCM/HMX access with the firmware compute-resource APIs, applies its own HMX
   power vote, and locks HMX. Pack operands into aligned VTCM, initialize the
   scale/bias pairs, execute the matrix instructions and read back the result.
   Ordinary host shared memory is not a replacement for the required VTCM
   tile buffers in this implementation.
6. **Verify and clean up every run.** Compare all results with an independent
   host oracle, check guards, unlock/release resources and remove the power
   client. Free the host buffer and close the remote handle. On this driver,
   successful last-handle close already tears down the session; repeating an
   explicit close produced error 44. Bound hardware tests with a child-process
   timeout and retain logs even on failure.

The executable implementation is in
[fastrpc_probe.c](../../src/tools/probe/fastrpc_probe.c) and
[fastrpc_probe_skel.c](../../src/tools/probe/fastrpc_probe_skel.c).
The isolated build and signed test harness are
[build.ps1](../../tools/whisper/build.ps1) and
[sign-fastrpc-probe.ps1](../../tools/whisper/sign-fastrpc-probe.ps1).
Use the reproduction commands below; the test harness handles staging,
catalog verification, search paths and timeouts. A successful build or catalog
verification alone is not a hardware execution test.

### Dependencies and deployment

At development time, HMX needs the extracted Hexagon SDK compiler/ABI headers
and the local Hexagon-enabled linker. At execution time, the project host and
DSP module need no QNN DLL, QNN stub, QNN skeleton, Python, compiler or SDK
library installation. They still require compatible Windows/OEM FastRPC,
driver and DSP firmware, plus an accepted module catalog. The vendor FastRPC
DLL itself imports UCRT; therefore the complete process is not libc-free even
though our host has no CRT imports. The HMX ELF has no `NEEDED` libraries, but
does resolve 11 weak firmware resource/power functions: it is not independent
of the firmware ABI.

The proven deployment is **development-signed on this configured machine**.
The user enabled TESTSIGNING and imported the dedicated public certificate;
HVCI remained enabled. This does not prove deployment on a stock retail
machine without those trust changes. The retail signing discussion later in
this document remains a separate, unresolved deployment question. Do not
disable signature enforcement or change boot/trust settings automatically.

### From a probe to a model engine

The next useful experiment is a separate resident DSP service that amortizes
RPC and resource-acquisition costs across many checked operations. It would
need tiled kernels for actual model shapes, tails and longer reductions;
explicit weight preparation/loading; supporting vector/scalar operators;
scratch/KV-cache ownership; cancellation and failure recovery; and a scheduler.
General FP16 numerics and any future quantized formats need their own tests.

Measure module load, RPC dispatch, packing/transfers and kernel execution
separately before comparing end-to-end model latency with QNN. Validate
intermediate tensors and final model outputs, then sustained and concurrent
operation. The current correctness probe acquires and releases resources per
matrix and is not a throughput benchmark. Keep Whisper, GLM-OCR and
TranslateGemma on their existing QNN paths until an independently validated
alternative meets their correctness and performance requirements.

### Independent Whisper CLI bring-up

The [whisper_cli.c](../../src/apps/whisper/whisper_cli.c) is separate from
the deployed QNN application. [whisper_wav.c](../../src/apps/whisper/whisper_wav.c) uses the
repository's `platform_open_read`, `platform_read`, `platform_seek` and
`platform_close` interfaces. It accepts mono 16 kHz IEEE float32 RIFF WAV,
counts all samples, and provides zero-padded 30-second windows at a 25-second
stride for inspection and log-mel probes. Transcription instead reads
consecutive 30-second segments without overlap, prints one line per segment,
and zero-pads the last one. This avoids duplicate overlap text but can split
words at boundaries; no timestamp alignment or word-level stitching exists.
The current CLI uses the Windows platform layer's ANSI file API; non-ANSI
paths are not yet supported. Classic RIFF data chunks are limited to 4 GiB.

From the repository root, with the existing native Windows ARM64 Clang and
`build/normal/linker.exe` available:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File experimental/snapdragon/tools/whisper/build.ps1 -StandaloneWhisper
./experimental/snapdragon/build/whisper-direct/whisper-cli.exe --inspect-wav experimental/snapdragon/data/bundestag-hearing-5min-16k-mono-f32.wav
./experimental/snapdragon/build/whisper-direct/whisper-convert.exe --model=tiny experimental/snapdragon/models/whisper-tiny/model.safetensors experimental/snapdragon/build/whisper-direct/tiny.wti
./experimental/snapdragon/build/whisper-direct/whisper-cli.exe --inspect-model experimental/snapdragon/build/whisper-direct/tiny.wti
./experimental/snapdragon/build/whisper-direct/whisper-cli.exe --probe-projection experimental/snapdragon/build/whisper-direct/tiny.wti
./experimental/snapdragon/build/whisper-direct/whisper-cli.exe --probe-mel experimental/snapdragon/data/long-form-35s.wav experimental/snapdragon/models/whisper-tiny/frontend-fp16
./experimental/snapdragon/build/whisper-direct/whisper-cli.exe --verify-mel experimental/snapdragon/models/calibration/fleurs/ar_eg-1606.wav experimental/snapdragon/models/whisper-tiny/frontend-fp16 experimental/snapdragon/models/whisper-tiny/frontend-fp16/fixture-log-mel-f32.bin
./experimental/snapdragon/build/whisper-direct/whisper-cli.exe --transcribe experimental/snapdragon/models/calibration/fleurs/de_de-1586.wav experimental/snapdragon/build/whisper-direct/tiny.wti experimental/snapdragon/models/whisper-tiny/frontend-fp16
```

The isolated builder compiles the CLI, WAV reader, C checkpoint converter and real
`src/platform/windows/core.c`, links with the in-tree PE ARM64 linker and
project-owned Windows import definitions, then runs synthetic no-CRT WAV and
tensor-index tests. Its compiler also uses `src/arch/aarch64/windows/chkstk.S`
for stack probes. The five-minute reference file reports 4,800,000 samples
and 12 windows. The PE imports Kernel32 and WS2_32 system DLLs because the
shared platform core also implements socket-capable I/O; it imports no CRT or
QNN. The input-path API is ANSI for now.

The converter reads the safetensors JSON table with bounded C parsing, checks
tensor names, F32/F16 types, shapes, offsets, non-overlap and complete data
coverage, and streams the original bytes without rounding or quantization.
It checks the chosen model's token/position embeddings and every encoder and
decoder layer's Q projection against the existing model descriptors. This is
not yet exhaustive validation of every Whisper operator tensor. The output is
published only after the complete write; the tool refuses an existing target.
Build artifacts live under `build/whisper-direct/`, not in `models/`.

The version-1 `.wti` file starts with a 64-byte little-endian header:
`WTINDEX1`, version (u32), model ID (u32), tensor count (u32), record size
(u32, currently 192), data offset (u64), data size (u64), table FNV-1a 64
(u64), data FNV-1a 64 (u64), and eight reserved zero bytes. Each 192-byte
record has a zero-terminated 96-byte ASCII name, type and rank (u32 each),
eight u64 dimensions (unused entries zero), data-relative start and end (u64
each), and tensor FNV-1a 64 (u64). The original tensor payload follows the
table. This format leaves room for later versioned type/quantization changes.
The pinned Tiny input produced 167
tensors and 151,042,560 unmodified payload bytes; independent SHA-256 comparison
of source and output payload passed. A Tiny input selected as Base and an
existing output path are rejected.

The [indexed reader](../../src/apps/whisper/whisper_indexed.c) checks the
header, model ID, table and whole payload hash on open, then verifies each
requested tensor's hash and size before returning its bytes. Its native tests
reject a corrupted table/payload, wrong model and undersized tensor buffer.
The `--probe-projection` command reads the real 384x384 FP32 weight and 384
bias for Tiny encoder layer zero's self-attention Q projection, applies a
deterministic 384-element input, and reports result bits and a fingerprint.
An independent calculation from the original safetensors file matched its
first four outputs within 0.00002; this synthetic input is an operation test,
not a real audio activation or model-quality result. The current scalar
matrix-vector routine is deliberately unoptimized and not an HMX kernel.
The `--probe-mel` mode reads every zero-padded 30-second WAV window with a
25-second stride, loads the Tiny double-precision Hann/DFT/mel constants from
the supplied frontend directory, and prints each channel-major log-mel
fingerprint. `--verify-mel` checks every value of a single-window float32
reference with tolerance 0.0001. The pinned FLEURS clip's 240,000 values all
passed; the largest difference was below one millionth. The isolated build
runs that reference gate when its local audio and fixture files are present.
These frontend binaries are local model data, not linked code dependencies.

The CPU [encoder](../../src/apps/whisper/whisper_cpu_encoder.c) loads verified
FP32 tensors from `.wti`, executes both convolutions, four self-attention/MLP
layers and residuals, then passes its pre-final-norm FP16 activations to the
existing [CPU decoder](../../src/apps/whisper/whisper_decoder.c). The decoder
applies that final norm, prepares cross-attention caches, greedily generates
German text and writes bytes via its local token table. Transcription also
needs `models/whisper-tiny/decoder-fp16/weights-fp16.bin` and
`token-bytes.bin`, existing generated model assets. They are **not** encoded
in `.wti` yet; a fresh checkout without them cannot transcribe. No Python,
QNN DLL, CRT, DSP service or HMX kernel runs in the CLI.

The pinned German FLEURS clip now produces intelligible but imperfect speech
text: it recognizes the slalom, first run, participants and same result but
mishears some words and numbers. A 35-second German recording emitted two
separate transcript lines; console output uses UTF-8. This demonstrates
functional transcription, **not** parity with the deployed QNN application
or acceptable accuracy on a broad corpus. The CLI forces the German
transcription prompt and uses a scalar encoder; no language detection,
timestamp stitching, optimized CPU inference or NPU model execution is
claimed. Compare intermediate activations and more transcripts before
replacing any operator with HMX.

## Verified HMX execution

### Reproduce

The HMX compiler is QuIC LLVM Hexagon Clang 19.0.07 from the native Windows ARM64
[Hexagon SDK 6.6.0.0 release archive](https://github.com/snapdragon-toolchain/hexagon-sdk/releases/download/v6.6.0.0/hexagon-sdk-v6.6.0.0-arm64-wos.tar.xz).
Its 857,553,272-byte archive matched the release's published SHA-256:

```text
CACEEBDB7C213C4840993176B9CDCE6946D8FF159D096B2EDD3685F0274BE477
```

The archive and extracted `6.6.0.0/` tree are in the ignored
`data/hexagon-toolchain/` directory. No installer or global environment change
was used. The HMX build reads `incs/` and `incs/stddef/` directly, without
vendoring headers into project source or linking SDK libraries. It requires
`-mv73 -mhmx`: the independent upstream LLVM 23.1.2 compiler rejects
`mxclracc.hf`, even with its Hexagon target enabled.

From the repository root, with the already-built local Hexagon-enabled LLVM
linker and the previously trusted development certificate:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File experimental/snapdragon/tools/whisper/build.ps1 -FastRpcProbe -FastRpcHmx
powershell.exe -NoProfile -ExecutionPolicy Bypass -File experimental/snapdragon/tools/whisper/sign-fastrpc-probe.ps1 -Hmx -Action Prepare
powershell.exe -NoProfile -ExecutionPolicy Bypass -File experimental/snapdragon/tools/whisper/sign-fastrpc-probe.ps1 -Hmx -Action Test
powershell.exe -NoProfile -ExecutionPolicy Bypass -File experimental/snapdragon/tools/whisper/sign-fastrpc-probe.ps1 -Hmx -Action Test -TinyIndexed experimental/snapdragon/build/whisper-direct/tiny-checked.wti
```

The corresponding VS Code tasks are `Snapdragon FastRPC HMX build`,
`Snapdragon FastRPC HMX sign` and `Snapdragon FastRPC HMX test`.
`-HexagonSdk` can select another extracted SDK root with the same tool layout.
Outputs are isolated in `build/fastrpc-hmx/`; the new catalog, payload and
receipt are in `data/fastrpc-signing-hmx/`. The original scalar signing artifacts
remain unchanged. HMX signing reuses the existing non-exportable key and public
certificate; it neither creates a certificate nor changes trust or boot policy.

`--hmx` is the explicit host option; the default remains host-only and
`--invoke` remains scalar-only. `--hmx-whisper <tiny.wti>` additionally exercises
a verified model weight tile; the helper selects it with `-TinyIndexed`.
Run hardware checks through the helper so each
child has an isolated module search path, adjacent catalog and 30-second limit.

### Kernel and oracle

Method 3 accepts two row-major 32x32 FP16 matrices in 4,096 input bytes and
returns a 2,096-byte reply: twelve diagnostic words plus 1,024 raw FP16 outputs.
The DSP checks the request, acquires 8 KiB of single-page VTCM with HMX access,
creates its own HMX power vote, and locks HMX on the calling thread. There are
no clock/DCVS changes, worker pools or HVX operations.

Each activation/weight/output tile occupies 2,048 bytes, aligned to 2,048 bytes.
Packing uses half-element index `(row / 2) * 64 + column * 2 + row % 2`;
the weight's logical row is the reduction dimension. The scale area is 256 bytes:
its first 128 bytes contain 32 FP16 **(scale=1, bias=0)** pairs, and the next
128 bytes are zero. Filling both halves of each pair with one incorrectly adds
one to every result; the initial identity test detected that error.

Between scalar memory barriers, the kernel loads `bias = mxmem2(...)`, executes
`mxclracc.hf`, issues paired `activation.hf = mxmem(...)` and
`weight.hf = mxmem(...)` loads with range 2047, and stores with
`mxmem(..., 0):after.hf = acc`. It unpacks the hardware result, checks the VTCM
tail guard, unlocks HMX, releases the resource, removes its power vote and
destroys its power client. Every cleanup status is returned and checked.
There is no DSP-side software matrix multiplication fallback.

The host independently computes ordinary row-major integer matrix products
and compares their exact FP16 representations with the returned values
(positive/negative zero are numerically equivalent). Fixtures cover left and
right identity, two dense signed matrices, zero, and a signed permutation.
Small integer operands keep sums exactly representable in FP16. All outputs
are poisoned before each call; inputs and the host/VTCM guard regions are
checked. This is not general FP16 rounding, overflow or inference-quality
coverage.
The updated suite also checks one 32x32 fractional fixture with absolute
error at most 0.0078125 against FP32 accumulation of its FP16 operands.
The optional Whisper test reads and verifies the full indexed Tiny checkpoint
and its layer-zero encoder self-attention Q weight before sending its top-left
32x32 tile to HMX. A deterministic fractional input matrix supplies 32 test
vectors. Its 1,024 outputs use the same host oracle and tolerance. This is
real **weight** data, not an encoder activation or a complete 384-wide Q
projection; quantization error against the original FP32 model output is not
established.

An important transport observation: malformed-input calls returned host
transport status zero even when the skeleton returned error 14, leaving the
poisoned reply untouched. Therefore transport success alone is insufficient.
For a valid reply buffer, malformed input now returns an explicit in-band
status 14, stage zero and executed zero. The hardware control requires that
rejection and an untouched product/guard area. CPU tests additionally exercise
short input, misalignment and undersized reply cases without executing HMX.

### Initial hardware receipt

All eight cases passed against the same MCDM driver described below:

| Case | Exit | Result |
| --- | --- | --- |
| Host-only | 0 | No DSP invocation |
| Scalar regression in the SDK-built module | 0 | Three correct transforms |
| Signed first / repeat / after controls | 0 each | Six exact matrices and bad-size rejection per session |
| Missing catalog / missing module / modified module | 9 each | Remote open rejected, session cleaned up |

Each matrix reports stage 9, executed 1, zero status, zero mismatches, zero
host/VTCM guard damage, and zero unlock/release/power-down/destroy status.
Each positive process reports `hmx.elements_verified=6144`,
`hmx_execution=verified` and a successful last-handle close. No child timed out.

Evidence is retained in
`data/fastrpc-hmx-tested-20260922-195746-7bd8189f/`: all per-case logs and binary
hashes, `signing-receipt.json`, `results.json` with `complete=true`, plus the
ELF import report and final linked-code disassembly. Exact signed identities:

```text
ELF: C2EC783C151870A6B8C24C170B796FBEEAAF3E8693C48BF8111F8D0944E082AE
CAT: A8F9E91BD73D6F3EC3426C93F10703BA97AF70BE1D47C42FE2F256D737EF9FB7
```

The linker is the existing upstream Hexagon-enabled `lld`. It warns about an
SDK-specific `.hexagon.attributes` tag; the final ELF's matrix code is decoded
with SDK `hexagon-llvm-objdump --mattr=+hmx`. The build requires all matrix
mnemonics in that disassembly, no ELF `NEEDED` entries, and only the allowlisted
weak `compute_resource_*` and `HAP_power_*` firmware imports. The final linked
ELF, not merely an assembly/object file, passed the hardware tests above.

### Fractional and Whisper-weight follow-up

The opt-in `-TinyIndexed` hardware suite passed nine cases on 2026-09-22:
host-only, scalar regression, three signed default HMX runs, three negative
catalog/module controls and one signed Tiny tile run. Each default signed
session checked seven tiles (7,168 values), including fractional FP16.
The model run checked those seven again plus 1,024 Tiny Q-weight tile results:
`hmx.whisper_tile.mismatches=0`, `hmx.whisper_tile.host_guards=0`,
`hmx.elements_verified=8192`, valid power/resource cleanup and final close.
All cases passed without timeouts. Evidence (logs, case results and hashes)
is in `data/fastrpc-hmx-tested-20260922-225749-593bfae2/`.
This is a hardware-backed bridge from a verified model artifact to the custom
HMX module, but the CLI still runs transcription entirely on CPU. The DSP
still acquires resources per 32x32 invocation; weight residency, tiling over
384 dimensions, bias/residual integration, encoder activation checks and
end-to-end NPU transcription remain to be built and measured.

The successful test despite zero HMX capability fields demonstrates that those
queries are not a reliable absence test on this installed driver. It does not
establish sustained throughput, concurrent-use behavior, arbitrary matrix
shapes, other devices, or a model backend. No performance claim is made.

## Verified scalar execution

Run the repeatable hardware suite from the repository root after building the
host probe and preparing/trusting the catalog as described below:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File experimental/snapdragon/tools/whisper/sign-fastrpc-probe.ps1 -Action Test
```

The seven-case suite stages independent host/module/catalog copies in fresh
directories, uses system-only PATH and a per-child `ADSP_LIBRARY_PATH`, and
limits each child to 30 seconds. It does not rebuild, re-sign, import trust,
install drivers or modify application artifacts.

| Case | Exit | Result |
| --- | --- | --- |
| Default host-only diagnostic | 0 | Host memory check passed, DSP not invoked |
| First signed invocation | 0 | Three changing-input transforms, no mismatches |
| Repeated signed invocation | 0 | Three transforms, no mismatches |
| Missing adjacent catalog | 9 | Remote open rejected |
| Missing module | 9 | Remote open rejected |
| Module with one appended byte, unchanged catalog | 9 | Remote open rejected |
| Signed invocation after the negative controls | 0 | Three transforms, no mismatches |

Every successful invocation run reports `remote_open.status=0`, three
`remote_invoke.status=0` and `remote_invoke.mismatches=0` pairs,
`remote_close.status=0`, `session_close=handled_by_last_handle`, and
`custom_dsp_execution=verified`. The suite verifies the cleanup paths as well
as exit codes. `hmx_execution=not_tested` remains correct: this is scalar Hexagon
execution, not matrix-accelerator or inference performance evidence.

Evidence is retained in
`data/fastrpc-tested-20260922-191857-56c35786/`: per-case logs and artifact
hashes, the signing receipt, and `results.json` with all seven checks passed
and `complete=true`. The signed ELF and catalog retain the SHA-256 identities
recorded in the signing section. Only the host cleanup logic changed.

The first trusted run already returned correct DSP results, but the probe then
incorrectly requested an explicit session close after successfully closing its
only remote handle. That returned 44 (`AEE_EINVHANDLE`) and caused exit 12.
Qualcomm's public `remote_handle64_close` implementation tears down the session
when its last multi-domain handle closes. The probe now uses that lifecycle,
retaining explicit session-close cleanup when open or handle close fails. It
does not blanket-ignore error 44. Initial and corrected logs are preserved in
`data/fastrpc-trusted-20260922-191622/`.

## Standalone probe

[fastrpc_probe.c](../../src/tools/probe/fastrpc_probe.c) is a separate freestanding
C executable. It has no SDK headers, QNN calls, standard C library calls or
third-party source. The only static import is Kernel32. Clang builds it with
`-ffreestanding -fno-builtin -nostdlib` and warnings as errors. The original
host-only ARM64 PE was 5,120 bytes; the extended probe remains Kernel32-only.

The probe:

1. Accepts an explicit absolute drive-letter DLL path, including quoted paths
   with spaces. It rejects missing, extra, overlong and unterminated arguments.
2. Uses `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` and
   `LOAD_LIBRARY_SEARCH_SYSTEM32`. It does not search the working directory or
   PATH for the supplied library's imports. The DLL must be trusted; loading an
   arbitrary DLL executes its initialization code.
3. Reports the actual loaded path and presence of ten FastRPC exports.
4. Calls `remote_handle_control(DSPRPC_GET_DSP_INFO, ...)` for cDSP domain 3,
   querying attributes 0 through 8. The ABI is three 32-bit fields, 12 bytes.
5. Allocates 4 KiB using `rpcmem_alloc(25, 1, 4096)`, obtains its descriptor,
   writes and reads a deterministic byte pattern through a volatile host view,
   and frees it. This is a **host** memory check, not a DSP round trip or a
   coherence test.
6. By default, unloads its library and reports custom DSP execution and HMX
   execution as `not_tested`.

With explicit `--invoke`, it additionally requests unsigned cDSP mode for its
own session using `remote_session_control(2, {3,1}, 8)`, opens the custom module,
and attempts three scalar calls with changing inputs and poisoned outputs.
It checks every result and a marker, closes any successfully opened handle,
and lets the last-handle close terminate its cDSP session. Failed-open and
failed-close paths retain an explicit session-close request. Only a fully
checked successful sequence can
report `custom_dsp_execution=verified`. The scalar mode does not reset the DSP,
change power votes or disable signature enforcement; its HMX status remains
`not_tested`. The explicit `--hmx` mode is described above.

[fastrpc_probe_skel.c](../../src/tools/probe/fastrpc_probe_skel.c) implements
the stateless integer transform. Method 2 takes exactly 64 input and 68 output
bytes, validating pointers, sizes and alignment. Host `remote_arg` is 16 bytes;
the DSP version is 8 bytes, checked at compile time. The scalar build imports no
functions and has no ELF `NEEDED` libraries. Its embedded host-only contract
test exercises three inputs plus malformed calls; this is not DSP evidence.

### Build and run

From the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File experimental/snapdragon/tools/whisper/build.ps1 -FastRpcProbe

$driver = Get-ChildItem "$env:SystemRoot\System32\DriverStore\FileRepository\qcnspmcdm8380.inf_arm64_*\libcdsprpc.dll"
$driver | Select-Object FullName, @{Name='Version'; Expression={$_.VersionInfo.FileVersion}}
```

Inspect the path/version first. If exactly one matching library is present:

```powershell
if (@($driver).Count -ne 1) { throw 'Select the intended installed driver explicitly' }
& ./experimental/snapdragon/build/fastrpc-probe/fastrpc_probe.exe $driver.FullName
$LASTEXITCODE
```

The VS Code **Snapdragon FastRPC probe build** task runs the same build command.
The existing [builder](../../tools/whisper/build.ps1) returns immediately after
building this probe and running its host scalar contract test; it does not
build or run the applications. The default
output directory is `build/fastrpc-probe/`. The mode rejects the ordinary build
root and combinations with other diagnostic modes or `-Clean`. Normal builds
are unchanged. No new build script or production dependency was introduced.

For automation, launch the probe as a child process with redirected stdout and
stderr, read both asynchronously, and enforce a time limit. The recorded tests
used `System.Diagnostics.Process`, `WaitForExit(30000)` and termination of only
that child on timeout. The native probe itself has no timeout around vendor
calls. No timeout occurred in the recorded runs.

Exit codes: 0 means the diagnostic completed and the host memory check passed;
2 is invalid CLI; 3 is DLL load failure; 4 is module-path reporting failure;
5 is missing memory API; 6 is allocation/descriptor/data failure; 7 is DLL
unload failure; 8 is missing invocation API or rejected session configuration;
9 is remote module open failure; 10 is invocation failure; 11 is incorrect
DSP output; 12 is remote handle/session cleanup failure; 90 is output failure.
With `--invoke`, exit 0 additionally requires all three calls, output checks,
and cleanup to succeed. Capability query errors are reported
individually and do not make the whole diagnostic fail. Only a query with
`.status=0` has a meaningful `.value`. In particular, exit 0 does not mean
unsigned module loading has been verified unless `--invoke` was requested.
Neither host-only nor scalar mode establishes HMX access; use the separate
`--hmx` variant and verification suite above.

### Build and attempt the DSP module

After building the compiler described below, use the VS Code **Snapdragon
FastRPC DSP build** task, or run from the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File experimental/snapdragon/tools/whisper/build.ps1 -FastRpcProbe -HexagonCompiler experimental/snapdragon/build/hexagon-llvm/bin/clang.exe
```

This builds `build/fastrpc-probe/fastrpc_probe_skel.so` with
`--target=hexagon-unknown-elf -mcpu=hexagonv73 -G0 -fPIC -ffreestanding` and
links it with LLD, `--no-undefined`, SysV hash and page-aligned RX/RW load
segments. No SDK headers, CRT objects or runtime libraries are linked.

The fixed module URI is:

```text
file:///fastrpc_probe_skel.so?fastrpc_probe_skel_invoke&_modver=1.0&_dom=cdsp
```

For a bounded attempt after selecting `$driver` as above:

```powershell
$directory = (Resolve-Path 'experimental/snapdragon/build/fastrpc-probe').Path
$info = New-Object Diagnostics.ProcessStartInfo
$info.FileName = Join-Path $directory 'fastrpc_probe.exe'
$info.WorkingDirectory = $directory
$info.Arguments = '"' + $driver.FullName + '" --invoke'
$info.UseShellExecute = $false
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$info.EnvironmentVariables['ADSP_LIBRARY_PATH'] = $directory
$info.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
$process = New-Object Diagnostics.Process
$process.StartInfo = $info
$null = $process.Start()
$stdout = $process.StandardOutput.ReadToEndAsync()
$stderr = $process.StandardError.ReadToEndAsync()
if (-not $process.WaitForExit(30000)) {
      $process.Kill()
      $process.WaitForExit()
      throw 'Probe timed out; only the child was terminated'
}
$stdout.Result
$stderr.Result
$process.ExitCode
$process.Dispose()
```

There is no certificate creation, catalog signing, trust-store modification,
driver installation or boot-policy change in the builder or probe.

## Compiler gate completed

Neither preinstalled Clang package had Hexagon. LLVM's 23.1.2 Windows release
build configuration also excludes it, so installing another ordinary Windows
binary package would not resolve this. Instead we downloaded the upstream
source archive and built native Windows ARM64 Clang and LLD with only the
Hexagon target, using the existing LLVM-MinGW compiler and Ninja.

Source archive: `data/hexagon-toolchain/llvm-project-23.1.2.src.tar.xz`, from
[LLVM 23.1.2](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.2).
Its verified SHA-256 is:

```text
C98BBEF08A2B4C2613CD50E9AA9AE7B69B1FE6C16B2C40373BC0AB6116FDF78A
```

Extracted components are `llvm`, `clang`, `lld`, `cmake`, `third-party` and
`libc`. LLVM 23 requires `libc` common-utility headers even with that runtime
disabled; this does not link libc into our probe or DSP module. All upstream
compiler material remains Git-ignored development storage, not application
source or a production dependency.

The reproducible configuration, after extracting these components, is:

```powershell
$cc = (Get-Command clang).Source.Replace('\','/')
$cxx = (Get-Command clang++).Source.Replace('\','/')
$source = 'experimental/snapdragon/data/hexagon-toolchain/llvm-project-23.1.2.src/llvm'
$build = 'experimental/snapdragon/build/hexagon-llvm'
$flags = @(
      '-G','Ninja','-S',$source,'-B',$build,'-DCMAKE_BUILD_TYPE=Release',
      "-DCMAKE_C_COMPILER=$cc","-DCMAKE_CXX_COMPILER=$cxx",
      '-DLLVM_ENABLE_PROJECTS=clang;lld','-DLLVM_TARGETS_TO_BUILD=Hexagon',
      '-DLLVM_DEFAULT_TARGET_TRIPLE=hexagon-unknown-elf',
      '-DLLVM_ENABLE_ZLIB=OFF','-DLLVM_ENABLE_ZSTD=OFF',
      '-DLLVM_ENABLE_LIBXML2=OFF','-DLLVM_ENABLE_LIBEDIT=OFF',
      '-DLLVM_INCLUDE_TESTS=OFF','-DLLVM_INCLUDE_BENCHMARKS=OFF',
      '-DLLVM_INCLUDE_EXAMPLES=OFF','-DCLANG_INCLUDE_TESTS=OFF',
      '-DCLANG_ENABLE_OBJC_REWRITER=OFF','-DCLANG_ENABLE_STATIC_ANALYZER=OFF',
      '-DLLVM_ENABLE_BINDINGS=OFF','-DLLVM_ENABLE_ASSERTIONS=OFF',
      '-DLLVM_PARALLEL_LINK_JOBS=1','-DLLVM_APPEND_VC_REV=OFF'
)
cmake @flags
if ($LASTEXITCODE) { throw 'Configuration failed' }
cmake --build $build --target clang lld --parallel 8
if ($LASTEXITCODE) { throw 'Compiler build failed' }
& "$build/bin/clang.exe" --version
& "$build/bin/clang.exe" --print-targets
```

Verified output is `clang version 23.1.2` and `hexagon - Hexagon`.
`LLVM_APPEND_VC_REV=OFF` prevents the enclosing newos Git repository from being
misreported as LLVM's source revision. The downloaded archive digest identifies
the compiler source. The development compiler itself uses the existing MinGW
runtime environment. No system compiler or PATH was replaced. Project `clean`
can remove this compiler's build directory; the archive/source remain in `data/`.

## Initial load results (historical)

| Experiment | Unsigned session | Remote open | Session close | Outcome |
| --- | --- | --- | --- | --- |
| Initial compact LLVM ELF | 0 | `0x80000406` | 0 | exit 9, no invocation |
| Page-aligned RX/RW ELF | 0 | `0x80000406` | 0 | exit 9, no invocation |
| Isolated executable with no DSP module | 0 | `0x80000406` | 0 | expected exit 9 |

`0x80000406` is `DSP_AEE_EOFFSET + AEE_EUNABLETOLOAD` (decimal 2147484678).
The initial and page-aligned modules have `EM_HEXAGON`, ELF32 little-endian,
V73 flags `0x73`, the exported invocation entry and no `NEEDED` dependencies.
Page alignment alone did not solve loading. Because a deliberately absent
module produces the same status, it does not prove that the present file was
found, passed host verification, or reached ELF loading on the DSP.

The independent Windows Hexagon backend's guide explicitly requires custom
HTP libraries to be covered by a trusted signed `.cat`; its CMake build has
an `inf2cat`/`signtool` path. This is an unmet prerequisite for our unsigned
artifact, not a decoded driver diagnosis. Those initial load attempts did not
install a trusted certificate, enable test-signing, disable Secure Boot, or
bypass the vendor verifier. The later user-enabled test-signing state and
prepared catalog are recorded below. The development-signed execution checks
above subsequently resolved this loader gate for the tested module and driver.

Local evidence directories:

- `data/fastrpc-execution-20260922-183933/`: initial ELF and failed load.
- `data/fastrpc-execution-20260922-184038/`: aligned ELF and failed load.
- `data/fastrpc-execution-20260922-184236/`: missing-module control, system-only
   PATH and an isolated executable directory containing spaces.
- `data/fastrpc-execution-20260922-184744/`: final LLVM build's ELF/import
   inspection, artifact hashes, eight CLI/DLL negative cases, host-memory test,
   reproduced loader failure and successful session cleanup (`validation.txt`).

These initial experiments also reran CLI rejection cases and the unchanged
host-memory path. They did not invoke the DSP; the later execution proof is
recorded above. No throughput or HMX result is claimed. Final module hashes
and ELF/import inspections are recorded in the final validation receipt; build
metadata can change when compiler provenance settings change.

## Self-signed development route on this machine

Investigated on 2026-09-22 at the user's request. This is a separate option
from the unchanged-retail-trust route below. It does **not** require an EV
certificate, Microsoft Hardware Developer enrollment or an external signing
partner. It does require deliberately configuring this machine to accept test
signatures. Test-signing, certificate trust and custom scalar DSP execution are
now verified for this development configuration.

### After reboot: catalog prepared

The user ran the administrator BCDEdit command and rebooted. A fresh successful
8-byte Code Integrity query returned `0x00283603`: test-signing (`0x2`) and
HVCI (`0x400`) are both active. The agent process remains unelevated.

[sign-fastrpc-probe.ps1](../../tools/whisper/sign-fastrpc-probe.ps1) is a
development-only helper with `Prepare`, `Verify` (default), explicit
administrator-only `Trust`, and opt-in hardware `Test` actions. It uses built-in Windows PowerShell/.NET
APIs, not an added runtime dependency of either executable.

The prepared identity and payload are:

```text
Subject: CN=newos FastRPC Probe Development
Thumbprint: 1ECB57EACB518BB25E503BBA7D29A16CED7963DA
Certificate expiry: 2026-12-21 (no timestamp service used)
Private key: RSA 3072, CurrentUser/My, CNG export policy None
Module SHA256: 79AE61F31AEB3D82D208812AD892020B1759A7B0B0053C90EC9B1293ABE044B3
Catalog SHA256: CE72431683946700BA2E2639405ECD75123F7807A130CF19390EC04DE18F22F3
```

The key is non-exportable and remains local. Only its public `.cer` is exported.
Artifacts live in `data/fastrpc-signing/`: `development.cer`,
`fastrpc_probe_skel.cat`, `receipt.json`, and `payload/fastrpc_probe_skel.so`.
The receipt records certificate, module and catalog hashes. The live build and
the existing QNN catalog were not modified. Re-running `Prepare` reuses the
recorded certificate and updates the isolated payload/catalog/receipt.

Verified before adding trust:

- SHA-256 file membership via `Test-FileCatalog`: valid.
- CMS signature mathematics and exact signer identity: valid.
- CNG private-key export policy: `None`.
- Authenticode trust: `UnknownError`, specifically an untrusted root certificate
   in the chain, not a signature-integrity failure.
- This certificate absent from Local Machine Root and TrustedPublisher.
- A deliberately wrong thumbprint is rejected before any trust-store change.

Those preparation checks alone did not establish FastRPC acceptance. The later
hardware suite now confirms that this PowerShell-generated catalog works beside
the module for the tested driver. The vendor's catalog was also inspected
successfully, and both use SHA-256 member hashing.

The user has completed this action in **PowerShell as Administrator**; the
command is retained for reproducibility and is not needed again for this key:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\Users\mathi\newos\experimental\snapdragon\tools\whisper\sign-fastrpc-probe.ps1" -Action Trust -ExpectedThumbprint 1ECB57EACB518BB25E503BBA7D29A16CED7963DA
```

The explicit thumbprint is pinned to the certificate inspected in this session,
not selected automatically from a certificate store. The helper first verifies
the artifact receipt, signature and membership, then imports the public
certificate into Local Machine Root and TrustedPublisher. It never invokes
UAC, changes boot policy, installs a driver, or imports a private key. Trust is
machine-wide for this certificate, not limited to the NPU module. The user ran
the administrator import; the agent independently verified the resulting trust.

The expected result after import is `AuthenticodeTrustStatus=Valid` and both
machine-store presence flags `True`. A separate unelevated check from the
repository root is:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File experimental/snapdragon/tools/whisper/sign-fastrpc-probe.ps1 -Action Verify
```

No extra reboot was needed for this certificate import. The candidate was then
staged beside an isolated host probe and successfully tested through remote
open/invoke/close. That scalar test did not establish HMX execution; the later
HMX hardware gate and its evidence are recorded above.

### Measured prerequisites

The following table records the **pre-reboot** investigation, not the current
test-signing state:

| Item | Observed state |
| --- | --- |
| OS | Windows 11 Home, ARM64, version 10.0.26200 |
| Secure Boot | Windows registry reports disabled (`UEFISecureBootEnabled=0`) |
| Active Code Integrity flags | `0x00003401` |
| Kernel Code Integrity | Enabled (`0x1`) |
| Test-signed content allowed | **No** (`0x2` absent) |
| HVCI / Memory Integrity | Enabled (`0x400`), strict mode (`0x1000`) |
| Isolated User Mode signing enforcement | Enabled (`0x2000`) |
| Current investigation process | Not elevated |
| Built-in certificate/catalog commands | `New-SelfSignedCertificate`, `New-FileCatalog`, `Test-FileCatalog`, `Set-AuthenticodeSignature` available |
| SDK SignTool | Not found in the standard Windows Kits paths during prior inventory |

The active flags were read using
`NtQuerySystemInformation(SystemCodeIntegrityInformation=103)`, with an 8-byte
`SYSTEM_CODEINTEGRITY_INFORMATION` structure; status was 0 and returned length
was 8. This distinguishes active test-signing policy from merely having Secure
Boot disabled. It does not inspect a pending BCD change for the next boot.

Microsoft documents test-signing with HVCI enabled, but requires signed content
rather than an unsigned-code shortcut. Do **not** disable Memory Integrity or
set `nointegritychecks` as part of this experiment. That Windows guidance is not
proof that this particular FastRPC module will load: catalog handling and the
DSP ELF/QuRT contract still need the actual remote execution test.

### Administrator step and scope

The user has now completed this boot prerequisite. For reproducibility, the
command is retained below; it cannot be applied by an unelevated session.
Run manually in **PowerShell as Administrator**, then restart Windows:

```powershell
bcdedit.exe /set '{current}' testsigning on
if ($LASTEXITCODE) { throw 'Test-signing change failed; do not assume it applied' }
```

Before changing boot settings, ensure that any device-encryption/BitLocker
recovery information is available privately. Do not paste recovery keys into
chat. No recovery-key query, boot change, trust import or reboot was performed
during that initial read-only investigation. If the command reports a policy restriction, stop
and inspect that restriction rather than disabling further protections.

This is an **OS-wide test-signing setting**, not permission confined to our
NPU probe. Windows may display a Test Mode watermark; applications or policies
that require normal signing enforcement may react to the change. Preserving
application files does not guarantee zero behavioral impact from a boot-policy
change. Windows Home was identified locally; no Home-specific exclusion was
found in the cited procedure, but success on this exact configuration is not
yet demonstrated.

After restart, confirm that the active Code Integrity `0x2` bit is set before
trying the module again. The preparation sequence, now completed, is:

1. Create a dedicated self-signed code-signing certificate for this probe.
   Prefer a non-exportable private key in the user's `My` store. Export only
   the public `.cer` for trust installation; there is no need to share a PFX
   or private key through chat.
2. Generate a SHA-256 catalog covering the exact built ELF. The reference
   backend uses an ARM64 INF with Inf2Cat and SignTool. Built-in PowerShell
   catalog/signature commands were used successfully with this driver; this
   does not establish equivalence for every driver-package submission scenario.
   Use isolated build output; do not modify or reuse the vendor QNN catalog.
3. Following the Windows Hexagon reference procedure, install only this
   certificate's public part into Local Machine Trusted Root Certification
   Authorities and Trusted Publishers, with explicit administrator approval.
   Record its thumbprint for exact rollback. This adds machine-wide trust for
   the certificate, not a sandboxed trust exception for one module.
4. Verify the catalog signature, exact module membership and unchanged hash.
   Establish catalog discovery/any required package staging without replacing
   the OEM driver. Re-sign the catalog whenever the module bytes change.
5. Run the existing bounded `--invoke` test, requiring successful open, three
   checked transforms, handle close and session close. Test-signing enabled or
   a valid catalog alone is not DSP execution proof. Keep HMX as a later gate.

To reverse the boot change, run in elevated PowerShell and restart:

```powershell
bcdedit.exe /set '{current}' testsigning off
if ($LASTEXITCODE) { throw 'Test-signing rollback failed' }
```

Separately remove only the experiment certificate's recorded thumbprint from
the stores where it was added, and remove any experiment-only staged package
if one was installed. Never remove a vendor certificate or driver as cleanup.
Disabling test-signing does not itself remove certificates from trust stores.

## Trusted signing route

Earlier follow-up on 2026-09-22, before the user chose the self-signed route.
During this retail-trust investigation no signing, upload, certificate purchase,
account enrollment, catalog installation or driver installation was performed.

### Evidence and prerequisites

`Get-AuthenticodeSignature` reports the deployed `build/libqnnhtpv73.cat` as
`Valid`, with this public signer identity:

```text
Subject: CN=Microsoft Windows Hardware Compatibility Publisher,
         O=Microsoft Corporation, L=Redmond, S=Washington, C=US
Issuer:  CN=Microsoft Windows Third Party Component CA 2014,
         O=Microsoft Corporation, L=Redmond, S=Washington, C=US
Signer thumbprint: 6022340C9F4CB00307DF8DB036D392ADAC766212
```

This establishes the signer on one working deployment, not that this driver
accepts only Microsoft signers or that any newly Microsoft-signed ELF will
load. Catalog signature validity alone also does not establish membership of
an arbitrary `.so`. The existing vendor catalog cannot authorize our different
module bytes.

The current-user `My` certificate store returned no code-signing certificates.
No `signtool.exe` was found below the standard Windows Kits 10 `bin` directories
in Program Files or Program Files (x86). This is not an inventory of external
HSM/cloud signing services, organization accounts or nonstandard SDK installs.

The read-only registry query of
`HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State` returned
`UEFISecureBootEnabled=0`: Windows currently reports Secure Boot disabled.
Earlier wording about preserving Secure Boot was a constraint, not a measured
claim that it was enabled. No firmware query requiring elevation was run.
At that stage we had not changed this state, queried/changed boot entries, or
established whether test-signing was enabled. The subsequent self-signed-route
investigation above measured active test-signing as disabled. This retail-trust
route, unlike the self-signed development option, must not depend on
disabling Secure Boot or adding a local test trust root; do not automatically
enable Secure Boot either, since boot configuration changes need user review.

Artifact SHA-256 identities checked during this follow-up:

```text
fastrpc_probe_skel.so:
79AE61F31AEB3D82D208812AD892020B1759A7B0B0053C90EC9B1293ABE044B3
libqnnhtpv73.cat (existing reference, not a catalog for our module):
C478ED7A5FD42484161C6914D8D19298B2CBE4A2F3A913B79D5534047DF45932
```

[llama.cpp PR 22306](https://github.com/ggml-org/llama.cpp/pull/22306), merged
on 2026-04-24, reports that its DSP-library package passed the Microsoft signing
pipeline and ran end-to-end after changing the INF destination to DIRID 13
(DriverStore). Its package is ARM64, file-copy-only, class ComputeAccelerator,
and contains Hexagon ELF libraries. This is a useful independent precedent,
not a Qualcomm/Microsoft commitment to accept this probe or proof of which
signing offering that submitter used.

Microsoft's documented Hardware Developer signing process requires an enrolled
account with an associated valid EV code-signing certificate. An associated
ordinary Authenticode certificate can sign individual submissions under the
documented rules; that does not remove the account's EV requirement. Buying a
certificate alone neither grants enrollment nor proves package eligibility.
The certificate signs the submission; Microsoft returns a newly signed catalog.

Enrollment also requires an organization's Microsoft Entra global
administrator, verified company information, an authorized legal contact,
agreement acceptance and Microsoft's approval. These cannot be inferred from
a local Windows account or performed on the user's behalf without the required
authority. No account or partner access was confirmed during this follow-up.

The current attestation offerings page lists supported PE/binary types, but
does not explicitly list ELF `.so` payloads. It describes attestation as a
testing offering, not retail WHCP certification. The external Hexagon signing
precedent therefore warrants asking about this exact package rather than
assuming that the ordinary attestation pipeline accepts it. Microsoft's newer
preproduction option can retain Secure Boot but requires explicit device trust
provisioning; it is not equivalent to an unchanged retail trust policy and is
not being applied here.

### Next steps and acceptance gates

1. Establish access to an existing Hardware Developer account/signing service,
   or a Qualcomm/OEM partner willing to submit the package. Do not buy a
   certificate until eligibility, legal publisher identity, cost and the
   supported signing offering for this ELF-only package have been confirmed.
2. Confirm the applicable offering with the dashboard/support. Microsoft's
   attestation documentation distinguishes testing scenarios from WHCP
   certification and Windows Update retail publication. A Microsoft signature
   is not a WHCP certification, and the precedent does not establish that our
   package qualifies for either route.
3. Freeze the exact module bytes and SHA-256 in an isolated submission
   directory. Prepare an original INF with the approved publisher identity,
   ARM64 architecture, catalog filename and DIRID 13 file-copy destination.
   Validate with current InfVerif/Inf2Cat; no fictitious device ID, service or
   driver replacement is needed merely to describe the library payload.
4. Create the submission CAB with the package in a subdirectory, sign through
   the authorized certificate provider, and submit from the authorized account.
   No private key, certificate password, hardware-token PIN or account token
   should be sent through chat. These are external identity/approval steps,
   not capabilities of the local compiler.
5. Verify the returned catalog's signature, certificate chain and membership
   for the exact ELF bytes. Preserve the returned package and hashes without
   rebuilding the module. Catalog creation or `/pa` signature verification
   alone is not proof of FastRPC acceptance.
6. Confirm with the provider whether this driver's supported route requires
   DriverStore staging or permits an adjacent catalog. Any privileged package
   installation needs separate approval and a user-run command; never replace
   the working Qualcomm driver or change boot/trust policy automatically.
7. Rerun the bounded open/invoke/close test with the unchanged security policy.
   Only successful remote loading and all three numerical checks establish
   custom DSP execution. The scalar ELF/QuRT compatibility gate is now verified
   by the development route above; the HMX gate has also passed with development
   signing, but neither result proves retail-trust deployment.

Until account/signing access and package eligibility are established, a locally
generated unsigned catalog or a new self-signed root would not complete this
trusted route. No submission-ready or signed package is claimed at this stage.

### Support request draft (not sent)

**Subject:** Microsoft-trusted catalog for a standalone Hexagon V73 ELF library
on Windows ARM64, without test-signing or custom device trust

We are developing an original, freestanding scalar FastRPC probe for a Surface
Laptop with Snapdragon X / Hexagon V73. The Windows host has no CRT or QNN
dependency of its own and calls the installed OEM `libcdsprpc.dll`. The DSP
payload is one ELF32 little-endian V73 shared object, with no ELF `NEEDED`
libraries, built by upstream LLVM 23.1.2. It is not a replacement Windows driver.

The installed NPU/MCDM library is version `30.0.0220.3000`. cDSP unsigned-session
configuration succeeds, but remote open returns `0x80000406` and cleanup
succeeds. The same status occurs with the module absent, so this is not a
confirmed signature-rejection diagnosis. The working vendor QNN catalog is
signed by Microsoft Windows Hardware Compatibility Publisher. Our own module
is currently unsigned; its SHA-256 is recorded above.

Please confirm:

1. Which Microsoft or Qualcomm/OEM signing offering accepts an ARM64
   ComputeAccelerator file-copy package containing only this Hexagon ELF
   payload? Is attestation available for this testing use, or is a different
   submission/partner process required?
2. What publisher enrollment, EV certificate, package metadata and validation
   requirements apply, including INF/InfVerif/Inf2Cat requirements for a
   DIRID 13 package without a `.sys` binary or Windows PDB?
3. Is there an approved route for an independent developer through a signing
   partner, and what are the eligibility requirements and costs?
4. For this driver, must the returned package be staged in DriverStore, or may
   the exact signed catalog and ELF be deployed together in an application
   directory? What catalog naming/discovery and verification policy applies?
5. Does the resulting signature work with Secure Boot enabled and ordinary
   retail trust, without test-signing, custom root certificates, preproduction
   policies, or replacement of the installed OEM driver?
6. Is an upstream-LLVM V73 ELF supported, and which loader diagnostics can
   distinguish catalog rejection, file discovery and ELF/QuRT incompatibility?

Reference precedent: https://github.com/ggml-org/llama.cpp/pull/22306 reports
successful Microsoft signing and execution for a similar Hexagon-library INF
package. We need confirmation for this package, not permission to reuse that
project's signature. Source and artifacts can be provided after confirming the
submission route; no upload or external support request has yet been made.

## Measured driver results

Both copies export `remote_handle64_open`, `remote_handle64_invoke`,
`remote_handle64_close`, `remote_handle_control`, `remote_session_control`,
`rpcmem_alloc`, `rpcmem_free`, `rpcmem_to_fd`, `remote_mmap64` and
`remote_munmap64`.

| Observation | NPU/MCDM copy | ADSP copy |
| --- | --- | --- |
| Driver-store directory | `qcnspmcdm8380.inf_arm64_553f7b0d71497f2c` | `qcadsprpc8380.inf_arm64_c177c9ef6a2f6580` |
| DLL version | `30.0.0220.3000` | `1.0.0.0` |
| cDSP support | 1 | query status 114 |
| Unsigned PD support | 1 | query status 114 |
| HVX 64-byte units | 0 | query status 114 |
| HVX 128-byte units | 4 | query status 114 |
| VTCM page bytes / count | 8,388,608 / 1 | query status 114 |
| Raw architecture value | 16,813,171 (`0x01008C73`) | query status 114 |
| HMX depth / spatial | 0 / 0, both queries succeed | query status 114 |
| Host allocation / descriptor / pattern / free | pass | pass |
| Probe exit | 0 | 0 |

The public host error header identifies 114 (`0x72`) as `AEE_ECONNREFUSED`, not
"API unsupported". This is an observed failure for this explicit ADSP DLL and
domain combination, not evidence that FastRPC is unavailable on the machine.

The architecture result is retained raw; its low byte is consistent with V73,
but the complete value should not be treated as a plain architecture number.
Unsigned-PD support is a capability report, not a successful module-load test.
VTCM size is not a reservation or a guarantee of available per-process memory.

The two successful zero-valued HMX queries **do not prove absence of HMX**.
They also do not prove access to it. The meaning/coverage of these fields on
this Windows driver needs confirmation against an actual matrix kernel and
the relevant resource-acquisition interfaces. Scalar/HVX execution alone will
not satisfy that gate.

SHA-256 identities of the tested `libcdsprpc.dll` files:

```text
MCDM: C0BD70C022C2DC78E579079E729EB209B3AA7876723A891C2B37F7BE34601DAF
ADSP: BC0AC7CFCD49E10C9891251F9288EE89FC2821182CD1AA200D32C6D5E47453C8
```

Local stdout/stderr and exit-code receipts are retained under
`data/fastrpc-probe-20260922/`, with one log per driver directory and negative
test. These are Git-ignored machine evidence; this document preserves the
conclusions independently of those files.

Validation passed for both real-driver runs and seven negative cases: no
argument, relative path, extra argument, unterminated quote, overlong argument,
missing DLL, and an existing DLL without the memory exports (`kernel32.dll`).
No application inference regression was run because application binaries and
code paths were not modified.

## Dependency boundary

The tested scalar execution architecture is:

```text
our no-CRT Windows ARM64 executable
  -> installed libcdsprpc.dll
  -> OEM Windows NPU driver and Hexagon firmware
  -> our Hexagon module and kernels
```

This eliminates the QNN HTP runtime, its stub and its DSP skeleton from
the probe's execution path, but it does **not** eliminate vendor dependencies.
Both tested FastRPC DLLs import Windows UCRT functions. No-CRT describes our
executable, not the entire loaded process. The DSP-side module also has a
different executable format/ABI from the Windows executable.

The MCDM library imports Windows `D3DKMT*` allocation, submission and
synchronization APIs, as well as trust-verification functions. This makes a
Linux FastRPC ioctl implementation an architectural reference, not a Windows
drop-in. Import inspection alone does not establish the private submission
protocol or exactly where signature checks apply. Replacing `libcdsprpc.dll`
would be a separate driver-protocol project; it is not necessary for the first
QNN-free kernel experiment.

Existing QNN context binaries remain opaque QNN artifacts. A replacement engine
would need a new kernel/model preparation path rather than loading those
contexts unchanged. Windows ML would move runtime/provider management to the
OS; it would not meet a no-external-inference-runtime objective.

## Remaining gates

Compiler, development catalog trust, module discovery, scalar execution, small
FP16 HMX matrix execution and cleanup gates are complete on this machine.
Remaining work is:

1. **Broaden kernel validation.** Cover general shapes, reduction lengths,
   FP16 numerical limits, persistent resources and concurrent-use behavior.
2. **Assess a model backend separately.** Measure dispatch cost, resident-buffer
   behavior, numerical correctness and sustained performance before considering
   replacing any of the three production inference paths.

## References

Consulted on 2026-09-22; external development branches can change. Only narrow
ABI declarations were represented locally, not a vendored implementation.
The HMX build additionally consumes headers from the extracted development SDK;
it does not link an SDK runtime or copy a third-party inference implementation.

- [Qualcomm FastRPC architecture and workflow](https://github.com/qualcomm/fastrpc)
- [Public remote API, query IDs and capability structure](https://github.com/qualcomm/fastrpc/blob/development/inc/remote.h)
- [Public shared-memory API](https://github.com/qualcomm/fastrpc/blob/development/inc/rpcmem.h)
- [Public error codes](https://github.com/qualcomm/fastrpc/blob/development/inc/AEEStdErr.h)
- [Public FastRPC handle-close/session lifecycle implementation](https://github.com/qualcomm/fastrpc/blob/development/src/fastrpc_apps_user.c)
- [tinygrad independent DSP backend](https://github.com/tinygrad/tinygrad/blob/master/tinygrad/runtime/ops_dsp.py): uses Clang freestanding Hexagon compilation and FastRPC, but explicitly excludes Windows and is not proof of V73 HMX access.
- [Windows Hexagon backend guide and catalog-signing prerequisite](https://github.com/ggml-org/llama.cpp/blob/master/docs/backend/snapdragon/windows.md)
- [Independent Windows FastRPC loading implementation](https://github.com/ggml-org/llama.cpp/blob/master/ggml/src/ggml-hexagon/htp-drv.cpp)
- [Independent catalog-generation build rules](https://github.com/ggml-org/llama.cpp/blob/master/ggml/src/ggml-hexagon/CMakeLists.txt)
- [Public HMX tile layout and instruction references](https://github.com/ggml-org/llama.cpp/blob/master/ggml/src/ggml-hexagon/htp/hmx-utils.h)
- [Public HMX scale/bias initialization call sites](https://github.com/ggml-org/llama.cpp/blob/master/ggml/src/ggml-hexagon/htp/matmul-ops.c)
- [Microsoft driver code-signing requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-reqs)
- [Microsoft attestation signing process and limitations](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation)
- [Hardware Developer Program enrollment and authority requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/hardware-program-register)
- [Microsoft signing offerings, including attestation and preproduction](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings)
- [Microsoft test-signing boot option, restart and HVCI requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option)
- [Active Code Integrity query and flag definitions](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation)
- [Hexagon library Microsoft-signing precedent](https://github.com/ggml-org/llama.cpp/pull/22306)
- [LLVM 23.1.2 Windows release target selection](https://github.com/llvm/llvm-project/blob/llvmorg-23.1.2/llvm/utils/release/build_llvm_release.bat)
- [Existing QNN platform notes](snapdragon-x-qnn.md)