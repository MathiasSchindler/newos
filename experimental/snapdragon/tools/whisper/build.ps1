param(
    [string]$Compiler = "clang",
    [string]$BuildDir = "experimental/snapdragon/build",
    [switch]$FastRpcProbe,
    [switch]$FastRpcHmx,
    [switch]$StandaloneWhisper,
    [string]$ProjectLinker = 'build/normal/linker.exe',
    [string]$HexagonSdk = '',
    [string]$HexagonCompiler = '',
    [switch]$DebugSymbols,
    [switch]$SelfFusionProbe,
    [switch]$SelfFusionCandidate,
    [switch]$GemmaGroup32Diagnostic,
    [ValidateSet('mapped', 'dequantize', 'expansion', 'legacy-block', 'composed')]
    [string]$GemmaGroup32Encoding = 'mapped',
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path
Push-Location $repoRoot
try {
    if ($StandaloneWhisper) {
        if ($FastRpcProbe -or $FastRpcHmx -or $HexagonSdk -or $HexagonCompiler -or
            $SelfFusionProbe -or $SelfFusionCandidate -or $GemmaGroup32Diagnostic -or
            $DebugSymbols -or $Clean -or $PSBoundParameters.ContainsKey('GemmaGroup32Encoding')) {
            throw 'StandaloneWhisper cannot be combined with other build modes or Clean'
        }
        if (-not $PSBoundParameters.ContainsKey('BuildDir')) {
            $BuildDir = 'experimental/snapdragon/build/whisper-direct'
        }
        if ([IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/') -eq [IO.Path]::GetFullPath('experimental/snapdragon/build')) {
            throw 'StandaloneWhisper requires an isolated build directory'
        }
    }
    if ($HexagonCompiler -and -not $FastRpcProbe) { throw 'HexagonCompiler requires FastRpcProbe' }
    if ($FastRpcHmx -and -not $FastRpcProbe) { throw 'FastRpcHmx requires FastRpcProbe' }
    if ($HexagonSdk -and -not $FastRpcHmx) { throw 'HexagonSdk requires FastRpcHmx' }
    if ($FastRpcHmx -and $HexagonCompiler) { throw 'Use HexagonSdk, not HexagonCompiler, for HMX' }
    if ($FastRpcProbe) {
        if ($Clean -or $DebugSymbols -or $SelfFusionProbe -or $SelfFusionCandidate -or
            $GemmaGroup32Diagnostic -or $PSBoundParameters.ContainsKey('GemmaGroup32Encoding')) {
            throw 'FastRpcProbe cannot be combined with other build modes or Clean'
        }
        if (-not $PSBoundParameters.ContainsKey('BuildDir')) {
            $BuildDir = if ($FastRpcHmx) { 'experimental/snapdragon/build/fastrpc-hmx' } else { 'experimental/snapdragon/build/fastrpc-probe' }
        }
        if ([IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/') -eq [IO.Path]::GetFullPath('experimental/snapdragon/build')) {
            throw 'FastRpcProbe requires a separate build directory'
        }
        if ($FastRpcHmx -and [IO.Path]::GetFullPath($BuildDir).TrimEnd('\', '/') -eq [IO.Path]::GetFullPath('experimental/snapdragon/build/fastrpc-probe')) {
            throw 'HMX must not overwrite the scalar probe directory'
        }
    }
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
    if ($StandaloneWhisper) {
        $linker = (Resolve-Path -LiteralPath $ProjectLinker -ErrorAction Stop).Path
        $sources = @(
            @('experimental/snapdragon/src/apps/whisper/whisper_cli.c', 'whisper_cli.obj'),
            @('experimental/snapdragon/src/apps/whisper/whisper_wav.c', 'whisper_wav.obj'),
            @('experimental/snapdragon/src/apps/whisper/whisper_convert.c', 'whisper_convert.obj'),
            @('experimental/snapdragon/src/apps/whisper/whisper_tensor_index.c', 'whisper_tensor_index.obj'),
            @('experimental/snapdragon/src/apps/whisper/whisper_artifact.c', 'whisper_artifact.obj'),
            @('experimental/snapdragon/src/apps/whisper/whisper_model.c', 'whisper_model.obj'),
            @('src/platform/windows/core.c', 'platform_core.obj'),
            @('experimental/snapdragon/src/apps/whisper/tests/whisper_wav_test.c', 'whisper_wav_test.obj'),
            @('experimental/snapdragon/src/apps/whisper/tests/whisper_tensor_index_test.c', 'whisper_tensor_index_test.obj')
        )
        New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
        $compileFlags = @('--target=aarch64-w64-windows-gnu', '-std=c11', '-Wall', '-Wextra',
            '-Wpedantic', '-Werror', '-O2', '-ffreestanding', '-fno-builtin',
            '-fno-stack-protector', '-ffunction-sections', '-fdata-sections',
            '-Isrc/shared', '-Iexperimental/snapdragon/src/apps/whisper')
        foreach ($source in $sources) {
            & $compilerPath @compileFlags -c $source[0] -o (Join-Path $BuildDir $source[1])
            if ($LASTEXITCODE -ne 0) { throw "Standalone WAV compilation failed: $($source[0])" }
        }
        & $compilerPath --target=aarch64-w64-windows-gnu -c src/arch/aarch64/windows/chkstk.S `
            -o "$BuildDir/chkstk.obj"
        if ($LASTEXITCODE -ne 0) { throw 'Standalone ARM64 stack-probe compilation failed' }
        $imports = @('src/platform/windows/imports/kernel32.def',
            'src/platform/windows/imports/ws2_32.def', 'src/platform/windows/imports/bcrypt.def')
        & $linker --target=pe-arm64 --gc-sections -o "$BuildDir/whisper-cli.exe" `
            "$BuildDir/whisper_cli.obj" "$BuildDir/whisper_wav.obj" "$BuildDir/platform_core.obj" @imports
        if ($LASTEXITCODE -ne 0) { throw 'Standalone WAV project link failed' }
        & $linker --target=pe-arm64 --gc-sections -o "$BuildDir/whisper-convert.exe" `
            "$BuildDir/whisper_convert.obj" "$BuildDir/whisper_tensor_index.obj" `
            "$BuildDir/whisper_artifact.obj" "$BuildDir/whisper_model.obj" `
            "$BuildDir/platform_core.obj" "$BuildDir/chkstk.obj" @imports
        if ($LASTEXITCODE -ne 0) { throw 'Standalone converter project link failed' }
        & $linker --target=pe-arm64 --gc-sections -o "$BuildDir/whisper-wav-test.exe" `
            "$BuildDir/whisper_wav.obj" "$BuildDir/whisper_wav_test.obj" @imports
        if ($LASTEXITCODE -ne 0) { throw 'Standalone WAV test link failed' }
        & ([IO.Path]::GetFullPath("$BuildDir/whisper-wav-test.exe"))
        if ($LASTEXITCODE -ne 0) { throw 'Standalone WAV window tests failed' }
        & $linker --target=pe-arm64 --gc-sections -o "$BuildDir/whisper-tensor-index-test.exe" `
            "$BuildDir/whisper_tensor_index.obj" "$BuildDir/whisper_tensor_index_test.obj" @imports
        if ($LASTEXITCODE -ne 0) { throw 'Standalone tensor-index test link failed' }
        & ([IO.Path]::GetFullPath("$BuildDir/whisper-tensor-index-test.exe"))
        if ($LASTEXITCODE -ne 0) { throw "Standalone tensor-index tests failed: $LASTEXITCODE" }
        Write-Output "Built $BuildDir/whisper-cli.exe and whisper-convert.exe (inference not implemented)"
        return
    }
    $compilerDirectory = Split-Path -Parent $compilerPath
    $dllTool = Join-Path $compilerDirectory "llvm-dlltool.exe"
    if (-not (Test-Path -LiteralPath $dllTool)) { throw "Could not find llvm-dlltool beside Clang" }

    if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
        Remove-Item -LiteralPath $BuildDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

    & $dllTool -m arm64 -d experimental/snapdragon/src/shared/imports/kernel32.def -l "$BuildDir/libkernel32.a"
    if ($LASTEXITCODE -ne 0) { throw "Failed to create the Kernel32 import library" }
    if ($FastRpcProbe) {
        $probeFlags = @(
            '--target=aarch64-w64-windows-gnu', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Oz',
            '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables',
            '-fno-asynchronous-unwind-tables', '-nostdlib', '-fuse-ld=lld',
            '-Wl,-e,mainCRTStartup', '-Wl,--no-insert-timestamp', '-Wl,-s', "-L$BuildDir", '-lkernel32'
        )
        & $compilerPath @probeFlags experimental/snapdragon/src/tools/probe/fastrpc_probe.c -o "$BuildDir/fastrpc_probe.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Failed to build fastrpc_probe.exe' }
        & $compilerPath @probeFlags -DFASTRPC_SKEL_TEST experimental/snapdragon/src/tools/probe/fastrpc_probe_skel.c -o "$BuildDir/fastrpc_skel_test.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Failed to build the scalar contract test' }
        & ([IO.Path]::GetFullPath("$BuildDir/fastrpc_skel_test.exe"))
        if ($LASTEXITCODE -ne 0) { throw 'Scalar contract test failed' }
        if ($HexagonCompiler -or $FastRpcHmx) {
            if ($FastRpcHmx) {
                if (-not $HexagonSdk) { $HexagonSdk = 'experimental/snapdragon/data/hexagon-toolchain/6.6.0.0' }
                $HexagonSdk = (Resolve-Path -LiteralPath $HexagonSdk).Path
                $sdkBin = Join-Path $HexagonSdk 'tools/HEXAGON_Tools/19.0.07/Tools/bin'
                $hexagonPath = Join-Path $sdkBin 'hexagon-clang.exe'
                $hexagonLinker = Join-Path $repoRoot 'experimental/snapdragon/build/hexagon-llvm/bin/lld.exe'
            } else {
                $hexagonPath = (Get-Command $HexagonCompiler -ErrorAction Stop).Source
                $hexagonLinker = Join-Path (Split-Path -Parent $hexagonPath) 'lld.exe'
            }
            if (-not (Test-Path $hexagonLinker)) { throw 'Build the local Hexagon-enabled lld.exe first' }
            $dspFlags = @(
                '--target=hexagon-unknown-elf', '-mcpu=hexagonv73', '-G0', '-std=c11', '-Wall', '-Wextra',
                '-Wpedantic', '-Werror', '-O2', '-fPIC', '-ffreestanding', '-fno-builtin',
                '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-nostdlib'
            )
            if ($FastRpcHmx) {
                $dspFlags = @($dspFlags | Where-Object { $_ -notin @('--target=hexagon-unknown-elf', '-mcpu=hexagonv73') })
                $dspFlags += @('-mv73', '-mhmx', '-DFASTRPC_HMX', '-isystem', "$HexagonSdk/incs", '-isystem', "$HexagonSdk/incs/stddef")
            }
            & $hexagonPath @dspFlags -c experimental/snapdragon/src/tools/probe/fastrpc_probe_skel.c -o "$BuildDir/fastrpc_probe_skel.o"
            if ($LASTEXITCODE -ne 0) { throw 'Hexagon probe compilation failed' }
            & $hexagonLinker -flavor gnu -shared --no-undefined --hash-style=sysv --no-rosegment `
                -z max-page-size=4096 -z separate-loadable-segments -z norelro `
                -soname fastrpc_probe_skel.so "$BuildDir/fastrpc_probe_skel.o" -o "$BuildDir/fastrpc_probe_skel.so"
            if ($LASTEXITCODE -ne 0) { throw 'Hexagon probe linking failed' }
            if ($FastRpcHmx) {
                $elf = & (Join-Path $sdkBin 'hexagon-readelf.exe') --wide --dyn-syms --dynamic "$BuildDir/fastrpc_probe_skel.so"
                if ($LASTEXITCODE) { throw 'HMX ELF inspection failed' }
                $elf | Set-Content -Encoding ASCII "$BuildDir/hmx-elf.txt"
                if ($elf -match '\(NEEDED\)') { throw 'HMX probe must not link external libraries' }
                $allowedImports = @('HAP_power_set', 'HAP_power_destroy_client', 'compute_resource_attr_init',
                    'compute_resource_attr_init_v2', 'compute_resource_attr_set_vtcm_param',
                    'compute_resource_attr_set_hmx_param', 'compute_resource_attr_get_vtcm_ptr',
                    'compute_resource_acquire', 'compute_resource_release', 'compute_resource_hmx_lock', 'compute_resource_hmx_unlock')
                foreach ($line in $elf) {
                    if ($line -match '^\s*\d+:\s+[0-9a-fA-F]+\s+\d+\s+\S+\s+(\S+)\s+\S+\s+UND\s+(\S+)') {
                        if ($Matches[1] -ne 'WEAK' -or $Matches[2] -notin $allowedImports) { throw ('Unexpected HMX import: ' + $line) }
                    }
                }
                $disassembly = & (Join-Path $sdkBin 'hexagon-llvm-objdump.exe') --mattr=+hmx -d "$BuildDir/fastrpc_probe_skel.so"
                if ($LASTEXITCODE) { throw 'HMX disassembly failed' }
                $disassembly | Set-Content -Encoding ASCII "$BuildDir/hmx-disassembly.txt"
                foreach ($instruction in @('mxclracc.hf', 'activation.hf', 'weight.hf', ':after.hf')) {
                    if (-not ($disassembly -match [regex]::Escape($instruction))) { throw ('Missing HMX instruction: ' + $instruction) }
                }
                Write-Output 'HMX_BUILD_AND_FIRMWARE_IMPORT_CHECK_PASS'
            }
            Write-Output "Built $BuildDir/fastrpc_probe_skel.so (custom Hexagon module)"
        }
        Write-Output "Built $BuildDir/fastrpc_probe.exe (no QNN; hardware probing is explicit)"
        return
    }
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
        "-Iexperimental/snapdragon/src/apps/whisper"
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
        "experimental/snapdragon/src/apps/whisper/main.c",
        "experimental/snapdragon/src/apps/whisper/whisper_artifact.c",
        "experimental/snapdragon/src/apps/whisper/whisper_model.c",
        "experimental/snapdragon/src/apps/whisper/whisper_frontend.c",
        "experimental/snapdragon/src/apps/whisper/whisper_decoder.c",
        "experimental/snapdragon/src/apps/whisper/whisper_decoder_qnn.c",
        "experimental/snapdragon/src/apps/whisper/whisper_encoder_qnn.c",
        "src/shared/math.c",
        "src/shared/runtime/memory.c",
        "src/shared/runtime/concurrency.c",
        "src/platform/windows/thread.c"
    )
    if ($SelfFusionProbe) {
        $probeSources = @($npuSources | Where-Object {
            $_ -notin @(
                'experimental/snapdragon/src/apps/whisper/main.c',
                'experimental/snapdragon/src/apps/whisper/whisper_decoder_qnn.c'
            )
        })
        & $compilerPath @flags '-Werror' `
            experimental/snapdragon/src/apps/whisper/benchmarks/self_fusion_probe.c `
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
        experimental/snapdragon/src/apps/whisper/benchmarks/decoder_kernel_benchmark.c `
        src/shared/runtime/memory.c src/shared/runtime/concurrency.c `
        src/platform/windows/thread.c `
        @npuLinkFlags -o "$BuildDir/decoder_kernel_benchmark.exe"
    if ($LASTEXITCODE -ne 0) { throw "Failed to build decoder_kernel_benchmark.exe" }
    Write-Output "Built $BuildDir/decoder_kernel_benchmark.exe"
} finally {
    Pop-Location
}