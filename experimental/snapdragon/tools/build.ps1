param(
    [string]$Compiler = "clang",
    [string]$BuildDir = "experimental/snapdragon/build",
    [switch]$DebugSymbols,
    [switch]$SelfFusionProbe,
    [switch]$SelfFusionCandidate,
    [switch]$GemmaGroup32Diagnostic,
    [ValidateSet('mapped', 'dequantize', 'expansion', 'legacy-block', 'composed')]
    [string]$GemmaGroup32Encoding = 'mapped',
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
Push-Location $repoRoot
try {
    if ($PSBoundParameters.ContainsKey('GemmaGroup32Encoding') -and -not $GemmaGroup32Diagnostic) {
        throw 'GemmaGroup32Encoding requires GemmaGroup32Diagnostic'
    }
    if ($GemmaGroup32Diagnostic) {
        if ($SelfFusionProbe -or $SelfFusionCandidate) { throw 'Gemma and Whisper candidate modes cannot be combined' }
        if (-not $PSBoundParameters.ContainsKey('BuildDir')) {
            $BuildDir = 'experimental/snapdragon/build/gemma-group32-probe'
        }
        if ([IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/') -eq [IO.Path]::GetFullPath('experimental/snapdragon/build')) {
            throw 'The Gemma diagnostic must use a separate build directory'
        }
    }
    if (($SelfFusionCandidate -or $SelfFusionProbe) -and -not $PSBoundParameters.ContainsKey('BuildDir')) {
        $BuildDir = if ($SelfFusionCandidate) { 'experimental/snapdragon/build/self-fusion-candidate' } else { 'experimental/snapdragon/build/self-fusion-probe' }
    }
    if ($SelfFusionCandidate -and
        [IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/') -eq [IO.Path]::GetFullPath('experimental/snapdragon/build')) {
        throw 'The experimental candidate must use a separate build directory'
    }
    $compilerCommand = Get-Command $Compiler -ErrorAction SilentlyContinue
    if (-not $compilerCommand) { throw "Could not find Clang: $Compiler" }
    $compilerPath = $compilerCommand.Source
    $compilerDirectory = Split-Path -Parent $compilerPath
    $dllTool = Join-Path $compilerDirectory "llvm-dlltool.exe"
    if (-not (Test-Path -LiteralPath $dllTool)) { throw "Could not find llvm-dlltool beside Clang" }

    if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
        Remove-Item -LiteralPath $BuildDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

    & $dllTool -m arm64 -d experimental/snapdragon/src/shared/imports/kernel32.def -l "$BuildDir/libkernel32.a"
    if ($LASTEXITCODE -ne 0) { throw "Failed to create the Kernel32 import library" }
    & $dllTool -m arm64 -d experimental/snapdragon/src/shared/imports/dxcore.def -l "$BuildDir/libdxcore.a"
    if ($LASTEXITCODE -ne 0) { throw "Failed to create the DXCore import library" }
    & $dllTool -m arm64 -d experimental/snapdragon/src/shared/imports/d3d12.def -l "$BuildDir/libd3d12.a"
    if ($LASTEXITCODE -ne 0) { throw "Failed to create the D3D12 import library" }
    & $dllTool -m arm64 -d experimental/snapdragon/src/shared/imports/directml.def -l "$BuildDir/libdirectml.a"
    if ($LASTEXITCODE -ne 0) { throw "Failed to create the DirectML import library" }

    $stackProbeObject = "$BuildDir/chkstk.obj"
    & $compilerPath --target=aarch64-w64-windows-gnu -c `
        src/arch/aarch64/windows/chkstk.S -o $stackProbeObject
    if ($LASTEXITCODE -ne 0) { throw "Failed to build the ARM64 Windows stack probe" }

    $flags = @(
        "--target=aarch64-w64-windows-gnu", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Oz",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables", "-ffunction-sections", "-fdata-sections", "-flto",
        "-Isrc/shared", "-Iexperimental/snapdragon/src/shared",
        "-Iexperimental/snapdragon/src/tools/probe",
        "-Iexperimental/snapdragon/src/tools/whisper"
    )
    if ($SelfFusionCandidate) {
        if ($SelfFusionProbe) { throw 'Choose either the full candidate or the one-layer probe' }
        $flags += '-DWHISPER_SELF_FUSION=1'
    }
    $linkFlags = @(
        "-nostdlib", "-fuse-ld=lld", "-Wl,-e,mainCRTStartup", "-Wl,-s", "-Wl,--gc-sections",
        "-Wl,--icf=safe", "-Wl,--no-insert-timestamp", "-Wl,/merge:.rdata=.text",
        "-Wl,--stack,1048576", $stackProbeObject, "-L$BuildDir", "-lkernel32", "-ldxcore", "-ld3d12", "-ldirectml"
    )
    & $compilerPath @flags experimental/snapdragon/src/tools/probe/main.c @linkFlags -o "$BuildDir/probe.exe"
    if ($LASTEXITCODE -ne 0) { throw "Failed to build probe.exe" }
    Write-Output "Built $BuildDir/probe.exe"

    $npuLinkFlags = @(
        "-nostdlib", "-fuse-ld=lld", "-Wl,-e,mainCRTStartup", "-Wl,-s", "-Wl,--gc-sections",
        "-Wl,--icf=safe", "-Wl,--no-insert-timestamp", "-Wl,/merge:.rdata=.text",
        "-Wl,--stack,1048576", $stackProbeObject, "-L$BuildDir", "-lkernel32"
    )
    if ($DebugSymbols) {
        $flags = @($flags | Where-Object { $_ -notin @('-fno-unwind-tables', '-fno-asynchronous-unwind-tables') })
        $flags += @('-g', '-gcodeview', '-funwind-tables', '-fasynchronous-unwind-tables')
        $npuLinkFlags = @($npuLinkFlags | Where-Object { $_ -ne '-Wl,-s' })
        $npuLinkFlags += '-Wl,--pdb='
    }
    $npuSources = @(
        "experimental/snapdragon/src/tools/whisper/main.c",
        "experimental/snapdragon/src/tools/whisper/whisper_artifact.c",
        "experimental/snapdragon/src/tools/whisper/whisper_model.c",
        "experimental/snapdragon/src/tools/whisper/whisper_frontend.c",
        "experimental/snapdragon/src/tools/whisper/whisper_decoder.c",
        "experimental/snapdragon/src/tools/whisper/whisper_decoder_qnn.c",
        "experimental/snapdragon/src/tools/whisper/whisper_encoder_qnn.c",
        "src/shared/math.c",
        "src/shared/runtime/memory.c",
        "src/shared/runtime/concurrency.c",
        "src/platform/windows/thread.c"
    )
    if ($SelfFusionProbe) {
        $probeSources = @($npuSources | Where-Object {
            $_ -notin @(
                'experimental/snapdragon/src/tools/whisper/main.c',
                'experimental/snapdragon/src/tools/whisper/whisper_decoder_qnn.c'
            )
        })
        & $compilerPath @flags '-Werror' `
            experimental/snapdragon/src/tools/whisper/benchmarks/self_fusion_probe.c `
            @probeSources @npuLinkFlags -o "$BuildDir/self-fusion-probe.exe"
        if ($LASTEXITCODE -ne 0) { throw "Failed to build self-fusion-probe.exe" }
        Write-Output "Built $BuildDir/self-fusion-probe.exe"
        return
    }
    & $compilerPath @flags "-DWHISPER_RUNTIME_ONLY" `
        "-Wno-unused-function" "-Wno-unused-variable" @npuSources `
        @npuLinkFlags -o "$BuildDir/npu_probe.exe"
    if ($LASTEXITCODE -ne 0) { throw "Failed to build npu_probe.exe" }
    Write-Output "Built $BuildDir/npu_probe.exe"

    $gemmaFlags = @()
    if ($GemmaGroup32Diagnostic) {
        $gemmaFlags += '-DGEMMA_GROUP32_DIAGNOSTIC=1'
        $encodingFlags = @{
            'dequantize' = '-DGEMMA_GROUP32_DEQUANTIZE=1'
            'expansion' = '-DGEMMA_GROUP32_EXPANSION=1'
            'legacy-block' = '-DGEMMA_GROUP32_LEGACY_BLOCK=1'
            'composed' = '-DGEMMA_GROUP32_COMPOSE=1'
        }
        if ($encodingFlags.ContainsKey($GemmaGroup32Encoding)) { $gemmaFlags += $encodingFlags[$GemmaGroup32Encoding] }
    }
    & $compilerPath @flags @gemmaFlags @npuSources `
        experimental/snapdragon/src/tools/probe/qnn_gemma_capabilities.c `
        @npuLinkFlags -o "$BuildDir/npu_probe_builder.exe"
    if ($LASTEXITCODE -ne 0) { throw "Failed to build npu_probe_builder.exe" }
    Write-Output "Built $BuildDir/npu_probe_builder.exe"

    & $compilerPath @flags `
        experimental/snapdragon/src/tools/whisper/benchmarks/decoder_kernel_benchmark.c `
        src/shared/runtime/memory.c src/shared/runtime/concurrency.c `
        src/platform/windows/thread.c `
        @npuLinkFlags -o "$BuildDir/decoder_kernel_benchmark.exe"
    if ($LASTEXITCODE -ne 0) { throw "Failed to build decoder_kernel_benchmark.exe" }
    Write-Output "Built $BuildDir/decoder_kernel_benchmark.exe"
} finally {
    Pop-Location
}