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
```

`data/`, `models/` and `build/` are local, Git-ignored storage. Their existing
contents were not relocated or deleted during the source reorganization: runtime
asset lookup, cache identities and historical report paths depend on them.
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

All commands below run from the repository root. Use `-BuildDir` for isolated
builds instead of overwriting an application that is open.

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

The installed paths are deliberately unchanged. A freshly built executable is
not automatically a deployed application: do not move it alone without its
required runtime, models and context bindings. QNN binaries include DLL, SO and
CAT files; keep their licenses and version record together. Prepared `.context`,
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

The reorganization was validated with all three native builds, both GUI builds,
Whisper mock/cleanup tests, 33 Python module tests, 7,470 tokenizer cases,
416 document cases, 323 numerical cases, 164 envelope corruption cases and OCR
artifact/prefill/generation regressions. No installed application was replaced.