param(
    [string]$Compiler = "clang",
    [string]$BuildDir = "experimental/snapdragon/build",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
Push-Location $repoRoot
try {
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

    & $dllTool -m arm64 -d experimental/snapdragon/src/imports/kernel32.def -l "$BuildDir/libkernel32.a"
    if ($LASTEXITCODE -ne 0) { throw "Failed to create the Kernel32 import library" }
    & $dllTool -m arm64 -d experimental/snapdragon/src/imports/dxcore.def -l "$BuildDir/libdxcore.a"
    if ($LASTEXITCODE -ne 0) { throw "Failed to create the DXCore import library" }
    & $dllTool -m arm64 -d experimental/snapdragon/src/imports/d3d12.def -l "$BuildDir/libd3d12.a"
    if ($LASTEXITCODE -ne 0) { throw "Failed to create the D3D12 import library" }
    & $dllTool -m arm64 -d experimental/snapdragon/src/imports/directml.def -l "$BuildDir/libdirectml.a"
    if ($LASTEXITCODE -ne 0) { throw "Failed to create the DirectML import library" }

    $flags = @(
        "--target=aarch64-w64-windows-gnu", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Oz",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables", "-ffunction-sections", "-fdata-sections", "-flto",
        "-Isrc/shared"
    )
    $linkFlags = @(
        "-nostdlib", "-fuse-ld=lld", "-Wl,-e,mainCRTStartup", "-Wl,-s", "-Wl,--gc-sections",
        "-Wl,--icf=safe", "-Wl,--no-insert-timestamp", "-Wl,/merge:.rdata=.text",
        "-Wl,--stack,1048576", "-L$BuildDir", "-lkernel32", "-ldxcore", "-ld3d12", "-ldirectml"
    )
    & $compilerPath @flags experimental/snapdragon/src/probe.c @linkFlags -o "$BuildDir/probe.exe"
    if ($LASTEXITCODE -ne 0) { throw "Failed to build probe.exe" }
    Write-Output "Built $BuildDir/probe.exe"

    $npuLinkFlags = @(
        "-nostdlib", "-fuse-ld=lld", "-Wl,-e,mainCRTStartup", "-Wl,-s", "-Wl,--gc-sections",
        "-Wl,--icf=safe", "-Wl,--no-insert-timestamp", "-Wl,/merge:.rdata=.text",
        "-Wl,--stack,1048576", "-L$BuildDir", "-lkernel32"
    )
    & $compilerPath @flags experimental/snapdragon/src/npu_probe.c `
        experimental/snapdragon/src/whisper_frontend.c `
        experimental/snapdragon/src/whisper_decoder.c `
        experimental/snapdragon/src/whisper_decoder_qnn.c src/shared/math.c `
        src/shared/runtime/memory.c src/shared/runtime/concurrency.c `
        src/platform/windows/thread.c `
        @npuLinkFlags -o "$BuildDir/npu_probe.exe"
    if ($LASTEXITCODE -ne 0) { throw "Failed to build npu_probe.exe" }
    Write-Output "Built $BuildDir/npu_probe.exe"
} finally {
    Pop-Location
}