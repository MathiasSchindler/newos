# Snapdragon X applications and tools

Freestanding Windows ARM64 applications and experiments for Snapdragon X Elite.
Native builds use Clang/LLD without CRT or libc. Python and PowerShell are
development tools, not application runtime dependencies.

## Directory layout

```text
src/
  apps/
    whisper/             Speech recognition engine, private modules, tests, benchmarks
    ocr/                 GLM-OCR engine, native GUI and private modules
    translate/           TranslateGemma CLI, native GUI, private modules and tests
  tools/probe/           Hardware/API probes and Gemma capability diagnostics
  shared/                Shared QNN ABI, Windows import definitions and QNN mock
tools/
  whisper/               Build, fetch, export, profile and test scripts for Whisper
  ocr/                   Build, export, reference and test tooling for GLM-OCR
  translate/             Build, export, reference and test tooling for TranslateGemma
  shared/                Hardware inventory, QNN runtime fetch, shared GUI tests
docs/
  apps/                  Application design, validation and performance records
  platform/              Snapdragon X, NPU, QNN and platform investigation
data/                    Original downloads, sample inputs and archived run evidence
models/                  Model weights, prepared artifacts and reference fixtures
build/                   Native binaries, QNN runtime, compiled contexts and scratch builds
distro/                  Independent minimal deployment folders; generated, Git-ignored
```

`data/`, `models/` and `build/` are local, Git-ignored storage. The central
build/clean entry point preserves non-rebuildable inputs outside `build/`, so
the complete build directory can be removed without deleting models.
There are no compatibility copies of the moved source files or scripts.

## Applications and shared code

| Component | Native entry points | Developer entry point |
| --- | --- | --- |
| Whisper | [main.c](src/apps/whisper/main.c) | [build.ps1](tools/whisper/build.ps1) |
| GLM-OCR | [ocr_model.c](src/apps/ocr/ocr_model.c), [ocr_gui.c](src/apps/ocr/ocr_gui.c) | [build-ocr.ps1](tools/ocr/build-ocr.ps1) |
| TranslateGemma | [translate.c](src/apps/translate/translate.c), [translate_gui.c](src/apps/translate/translate_gui.c) | [build-gemma.ps1](tools/translate/build-gemma.ps1) |
| Hardware probes | [main.c](src/tools/probe/main.c) | Whisper build also builds diagnostic tools |

Application-specific code stays with its application, including native tests and
benchmarks. TranslateGemma's [gemma_runner.c](src/apps/translate/gemma_runner.c)
contains its private QNN runner and diagnostic modes; it is no longer included
from a developer test directory. This reorganization does not change inference.

Only genuinely shared native infrastructure belongs in `src/shared/`: the
[QNN ABI](src/shared/qnn_abi.h), Windows import definitions and QNN test mock.
Generic math, crypto, compression and runtime support are reused directly from
the repository's [src/shared](../../src/shared), rather than copied here.
The [native GUI test driver](tools/shared/test-translate-gui.py) covers both OCR
and TranslateGemma. No separate Whisper GUI is currently present.

## Data, models and platform documentation

| Material | Location or entry point |
| --- | --- |
| Hardware, drivers, NPU and QNN | [Snapdragon X / QNN](docs/platform/snapdragon-x-qnn.md), [NPU investigation](docs/platform/npu-plan.md) |
| QNN-free FastRPC investigation | [Standalone probe, measured capabilities and remaining gates](docs/platform/fastrpc-without-qnn.md) |
| OCR architecture and measured limits | [OCR](docs/apps/ocr.md) |
| Translation architecture and measured limits | [TranslateGemma](docs/apps/translate.md) |
| Whisper timing and diagnostics | [Benchmarks](docs/apps/whisper-benchmark.md), [Diagnostics](docs/apps/whisper-diagnostics.md) |
| Download and artifact recipes | Versioned `*-model*.json` catalogs beside each application's developer scripts |
| Original model downloads and SDK archive | `data/translategemma-4b/`, `data/v2.50.0.260828.zip`; model-specific source packages under `models/` |
| Prepared weights and fixtures | `models/whisper-*`, `models/glm-ocr-*`, `models/translategemma-*` |
| Measurements and preserved earlier releases | Named run/archive directories under `data/`; do not overwrite their evidence |
| Deployed library licenses and version | `build/qnn-licenses/`, `build/qnn-runtime-version.txt` |

Use [fetch-qnn-runtime.ps1](tools/shared/fetch-qnn-runtime.ps1) for runtime
extraction and [inventory.ps1](tools/shared/inventory.ps1) for local hardware
and driver information. Existing quality limitations remain documented in the
application records; reorganizing files is not a new model-quality acceptance.

## Binaries and builds

From this directory, use [make.cmd](make.cmd) on Windows:

```powershell
.\make.cmd                 # Build all three applications and both GUIs
.\make.cmd clean           # Preserve inputs, then remove the entire build directory
.\make.cmd rebuild         # Clean followed by build
.\make.cmd clean -WhatIf   # Preview without changing files
.\make.cmd status          # Inventory and processes blocking clean/build
.\make.cmd test            # Isolated tests of the clean/preservation contract
```

The equivalent PowerShell entry is [make.ps1](make.ps1), for example
`./make.ps1 clean`. The CMD wrapper uses a process-local execution-policy bypass;
it does not change machine or user policy. VS Code tasks **Snapdragon build**,
**Snapdragon clean** and **Snapdragon rebuild** invoke the same implementation.
Commands work independently of the current working directory.

### Portable distributions

```powershell
.\make.cmd distro                 # Build and package all three applications
.\make.cmd distro -WhatIf         # List the allowlist without building/copying
.\make.cmd test-distro            # Small synthetic packaging contract tests
.\make.cmd distro -DistroDir C:\releases\snapdragon-x
```

Each of `distro/whisper/`, `distro/translate/` and `distro/ocr/` is independent:
copy the entire application folder, without the checkout or `build/`. Each has
its own `bin/`, `models/`, licenses, README and SHA-256 `manifest.json`. The
manifest identifies the purpose and exact size/hash of every shipped file
except itself. Documentation and licenses are explicitly distinguished from
inference inputs. No launch script, Python, CRT, SDK installation or compiler
is needed at runtime. Both existing GUIs are included; Whisper is a CLI.

| Package | Runtime/model inputs | Packaged size |
| --- | --- | --- |
| Whisper | Medium CLI, default m1c self-fused NPU context, CPU decoder weights and token bytes | 2.32 GiB / 16 files |
| TranslateGemma | W8 CLI/GUI, three shared 512-token partition bundles, selection context, tokenizer, embedding and four positional tables | 4.31 GiB / 25 files |
| GLM-OCR | Engine/GUI, 27 vision, 18 text and 10 generation `.got` files | 2.17 GiB / 70 files |

Counts include runtime support and legal/documentation metadata. Whisper and
TranslateGemma need only `QnnHtp.dll`, `QnnHtpV73Stub.dll`, the V73 skeleton SO
and CAT. OCR additionally needs `QnnHtpPrepare.dll`. OCR builds its disposable
`bin/ocr-app/graph-cache/` on first use; its GUI creates `runs/` beside the
engine. Neither cache nor run output is shipped. Keep the OCR folder writable.
The CLI requires a pre-created empty output directory, as shown in its README.

The main `build/translate.exe` and `build/translate-gui.exe`, as well as
`distro/translate/`, now use 4B W8 automatically. A separate W8 executable is
not needed. See [translation runtime notes](docs/apps/translate.md#installed-w8-runtime)
for validation, remaining quality limitations and the W4 rollback location.

Whisper's runtime model/context paths are executable-relative; audio paths
remain relative to the caller. The developer builder retains its old lookup
rules. TranslateGemma uses a generated five-entry binding with paths relative
to that binding; the original binding and models are not modified. Whisper
ships Medium/default offload only. Alternate models, graph building,
TranslateGemma's legacy decode modes and diagnostics need development assets
and are outside the minimal packages.

`distro` uses the existing build/preservation lock and build commands. It copies
an explicit allowlist, checks source/copy hashes and publishes only completed
staging output. It refuses an existing destination: use a new `-DistroDir` for
another release. `clean` does not delete distributions. A dry-run needs the
inputs already restored in `build/`; a real distro build restores them normally.
No original model download or conversion is started.

These packages target **Windows 11 ARM64, Snapdragon X / Hexagon V73**, with a
compatible OEM Qualcomm NPU/FastRPC driver installed. Windows system DLLs and
driver files are not bundled. The Qualcomm DLLs import Windows UCRT APIs, and
the V73 stub imports the driver's `libcdsprpc.dll`; the application executables
themselves have no CRT imports. The three Qualcomm DLLs are not a substitute
for that driver. Other NPU generations and arbitrary driver versions are not
guaranteed; another physical machine has not been tested. Existing model quality
limitations are unchanged. The model cards and Qualcomm license/notice files
are retained; review their full redistribution terms before public release.

The VS Code **Snapdragon distro inference tests** task, or
`tools/shared/test-distro.ps1`, verifies manifest hashes and copies only listed
files into a temporary directory outside the checkout (with spaces in its
path). It runs all three applications from an empty working directory with a
system-only `PATH`, without QNN SDK environment variables; sample inputs and
test logs are kept separate from distribution contents. Missing Whisper
contexts and translation embeddings must fail rather than use checkout assets.
Reports are preserved under `data/distro-verification-*/`. This development
test uses the existing development Python interpreter for native GUI automation;
it does not add any application runtime dependency. The relocation check also
opens both GUIs, translates a greeting in the translation GUI, and loads an
image preview in the OCR GUI. OCR inference is checked through the engine CLI.
The complete relocated run passed on 2026-09-21, including cold OCR graph-cache
creation and intentional 32-token termination (exit 3). Its receipt is
`data/distro-verification-b3888cb1078b46ba94def5abfd5e6885/results.json`.

Build requires Clang/LLVM and the pinned QAIRT archive in
`data/v2.50.0.260828.zip`. It extracts and verifies the QNN runtime locally,
then compiles Whisper, TranslateGemma CLI/GUI and the production OCR engine/GUI.
The runtime is centralized in `build/`; application and model subdirectories do
not carry private copies. This V73 machine uses three Qualcomm DLLs:
`QnnHtp.dll`, `QnnHtpV73Stub.dll` and `QnnHtpPrepare.dll`. The first two are the
minimum for restored inference. `QnnHtpPrepare.dll` remains because OCR can
rebuild its graph cache. `QnnSystem.dll` and the V81 runtime are not deployed.
The matching `libQnnHtpV73Skel.so` and `libqnnhtpv73.cat` support files remain
required even though they are not Windows DLLs.
Whisper uses the deployed Self-Fusion variant matching the preserved Medium
context; it is built under `build/whisper/` and its executables are installed at
the existing build-root paths.
No model download, model conversion or quality campaign is started. Existing
prepared model/context artifacts are required for inference; build does not
invent missing ones. OCR can regenerate its disposable graph cache as needed.

### Clean and preserved inputs

`clean` removes **all of `build/`**, not only executables. Before deletion it
preserves non-rebuildable files in `data/build-state/files/` and records their
SHA-256 hashes in `data/build-state/manifest.json`. This includes prepared
Gemma/Whisper contexts and bindings, original-looking or unknown model files,
development Python environments, OCR input images/runs and other evidence.
Unchanged saved files are reused. Changed older versions are retained under
`data/build-state/history/` as short archive filenames with JSON sidecars recording
the original path and hash; preservation is deliberately conservative.

Reproducible native binaries, objects, import libraries, Python bytecode caches
and QNN runtime copies are discarded. Old experimental OCR `.qob`/`.qoc` caches are disposable; the
deployed `ocr-app/graph-cache/` is preserved to avoid unnecessary recompilation.
`models/`, the SDK archive and existing data archives are never deletion targets.
The first preservation requires additional disk space and time; it is not an
attempt to purge every historical artifact from the machine.

The next build restores the default Gemma 512-token binding/contexts, root
Whisper contexts, deployed OCR cache/input/runs and development environments.
Other experimental artifacts and reports remain in the preserved tree instead
of repopulating `build/`. Restores are hash-checked, and an already present build
file is not overwritten by a saved copy. Do not delete `data/build-state/` unless
its retained inputs and evidence are no longer needed.

Clean/build refuse active processes using the build tree and reject junctions
or symbolic links rather than following them. Close application windows before
running either operation. A preservation failure prevents deletion; if a later
compile fails, fix the reported prerequisite and rerun build. The clean contract
tests cover full removal, model preservation, selective restore, version history,
corruption, path traversal and reparse-point rejection using synthetic files.
The last operation's log and explicit completion/error status are recorded in
`data/build-state/last-operation.log` and `last-operation.json`.

The full clean/rebuild was exercised on 2026-09-17: 102,106 files (11.49 GiB of
inputs, development dependencies and evidence) were preserved from a 23.04 GiB
build tree, the build directory was completely removed, and all three native
applications plus both GUIs were rebuilt successfully. Preserving large existing
development environments takes time; the initial migration is not a quick delete.
After matching Whisper's deployed Self-Fusion configuration, the rebuilt
TranslateGemma produced the exact reference translation, OCR verified all 26
Vision artifacts, and Whisper transcribed the 35-second sample with the unchanged
transcript hash and 6,384 NPU self-attention submissions. The final report is
`data/rebuild-verification-1199df64e0284cd2ad9495de7cfda07a/results.json`.

### Individual builds

The following lower-level commands run from the repository root. Use `-BuildDir`
for isolated builds instead of overwriting an application that is open. Their
individual switches are not substitutes for the central model-preserving clean.

```powershell
& ./experimental/snapdragon/tools/whisper/build.ps1 -BuildDir experimental/snapdragon/build/layout-check/whisper
& ./experimental/snapdragon/tools/translate/build-gemma.ps1 -Translate -Test -BuildDir experimental/snapdragon/build/layout-check/translate
& ./experimental/snapdragon/tools/translate/build-gemma.ps1 -Gui -BuildDir experimental/snapdragon/build/layout-check/translate
& ./experimental/snapdragon/tools/ocr/build-ocr.ps1 -Generate -LargeImages -ReuseDecode -GraphCache -AppMode -Test -BuildDir experimental/snapdragon/build/layout-check/ocr
& ./experimental/snapdragon/tools/ocr/build-ocr.ps1 -Gui -BuildDir experimental/snapdragon/build/layout-check/ocr
```

| Installed application | Executable | Required nearby artifacts |
| --- | --- | --- |
| Whisper | `build/npu_probe.exe` | QNN runtime and model-specific contexts; weights under `models/` |
| TranslateGemma | `build/translate.exe`, `build/translate-gui.exe` | `build/gemma-block/`, QNN runtime; tokenizer and weights under `models/` |
| OCR | `build/ocr-app/ocr-generate.exe`, `build/ocr-app/ocr-gui.exe` | `build/ocr-app/graph-cache/`, parent QNN runtime and model assets |

The application paths are deliberately unchanged. A freshly built executable is
not automatically a deployed application: do not move it alone without its
required runtime, models and context bindings. Keep the centralized V73 DLL, SO
and CAT files with their licenses and version record. Prepared `.context`,
`.qnnctx` and `.qob` files belong to a specific graph/runtime identity and are not
interchangeable model weights. Python environments under `build/` are retained
development dependencies, not application binaries.

## Layout verification

Run the VS Code task **Snapdragon check layout**, or:

```powershell
& ./experimental/snapdragon/tools/shared/inventory.ps1 -CheckLayout
```

This checks directory ownership, native include paths, PowerShell/Python syntax,
documentation links, catalog JSON and source/script references in VS Code tasks. It does not download
models, execute inference or regenerate caches. `-Python` selects another
development interpreter.

The source reorganization was validated with all three native builds, both GUI builds,
Whisper mock/cleanup tests, 33 Python module tests, 7,470 tokenizer cases,
416 document cases, 323 numerical cases, 164 envelope corruption cases and OCR
artifact/prefill/generation regressions. These are separate from the clean
contract tests above.