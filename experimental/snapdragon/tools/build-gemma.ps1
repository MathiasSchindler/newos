param(
    [string]$Compiler = 'clang',
    [string]$BuildDir = 'experimental/snapdragon/build',
    [string]$ModelDir = 'experimental/snapdragon/models/translategemma-4b-stage4',
    [switch]$Test,
    [switch]$ExportReference,
    [switch]$TestNumerics,
    [switch]$ExportNumerics,
    [switch]$TestBlocks,
    [switch]$TestPrompt,
    [switch]$RestorePrompt,
    [ValidateSet(512, 1024, 2048)][int]$PromptBucket = 512,
    [ValidateSet(4, 8)][int[]]$BlockBits = @(8, 4),
    [ValidateSet(0, 5)][int[]]$BlockLayers = @(0, 5),
    [string]$NumericDir = 'experimental/snapdragon/models/translategemma-4b-stage5-v2',
    [string]$Python = 'experimental/snapdragon/build/calibration-venv/Scripts/python.exe'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$originalBlasThreads = $env:OPENBLAS_NUM_THREADS
Push-Location $repoRoot
try {
    if ($ExportNumerics) {
        $env:OPENBLAS_NUM_THREADS = '4'
        & $Python experimental/snapdragon/tools/export-translategemma.py `
            --numerical-reference --replace --output $NumericDir
        if ($LASTEXITCODE -ne 0) { throw 'Numerical reference export failed' }
    }
    if ($ExportReference) {
        & $Python experimental/snapdragon/tools/export-translategemma.py `
            --tokenizer-only --tokenizer-reference --replace --output $ModelDir
        if ($LASTEXITCODE -ne 0) { throw 'Tokenizer reference export failed' }
    }
    $compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
    $compilerDirectory = Split-Path -Parent $compilerPath
    $dllTool = Join-Path $compilerDirectory 'llvm-dlltool.exe'
    $readObj = Join-Path $compilerDirectory 'llvm-readobj.exe'
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    & $dllTool -m arm64 -d experimental/snapdragon/src/shared/imports/kernel32.def -l "$BuildDir/libkernel32.a"
    if ($LASTEXITCODE -ne 0) { throw 'Kernel32 import library creation failed' }
    & $compilerPath --target=aarch64-w64-windows-gnu -c src/arch/aarch64/windows/chkstk.S -o "$BuildDir/gemma-chkstk.obj"
    if ($LASTEXITCODE -ne 0) { throw 'ARM64 stack probe compilation failed' }
    $flags = @(
        '--target=aarch64-w64-windows-gnu', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Oz',
        '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables',
        '-fno-asynchronous-unwind-tables', '-ffunction-sections', '-fdata-sections', '-flto',
        '-Isrc/shared', '-Iexperimental/snapdragon/src/shared', '-Iexperimental/snapdragon/src/tools/gemma',
        '-nostdlib', '-fuse-ld=lld', '-Wl,-e,mainCRTStartup', '-Wl,-s', '-Wl,--gc-sections',
        '-Wl,--icf=safe', '-Wl,--no-insert-timestamp', '-Wl,/merge:.rdata=.text',
        '-Wl,--stack,1048576', "$BuildDir/gemma-chkstk.obj", "-L$BuildDir", '-lkernel32'
    )
    $binary = "$BuildDir/test-gemma-tokenizer.exe"
    & $compilerPath @flags experimental/snapdragon/tools/test-gemma-tokenizer.c `
        experimental/snapdragon/src/tools/gemma/gemma_model.c `
        experimental/snapdragon/src/tools/gemma/gemma_artifact.c `
        experimental/snapdragon/src/tools/gemma/gemma_numeric.c `
        experimental/snapdragon/tools/test-gemma-numeric.c `
        experimental/snapdragon/src/tools/gemma/gemma_tokenizer.c src/shared/crypto/sha256.c -o $binary
    if ($LASTEXITCODE -ne 0) { throw 'Freestanding tokenizer compilation failed' }
    $audit = & $readObj --file-headers --coff-imports $binary | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'Tokenizer PE audit failed' }
    $imports = @([regex]::Matches($audit, '(?m)^\s*Name: (.+\.dll)\s*$') | ForEach-Object { $_.Groups[1].Value.Trim() })
    if ($audit -notmatch 'IMAGE_FILE_MACHINE_ARM64' -or $imports.Count -ne 1 -or $imports[0] -ine 'KERNEL32.dll' -or
        $audit -notmatch 'ExceptionTableRVA: 0x0\b' -or $audit -notmatch 'ExceptionTableSize: 0x0\b' -or
        $audit -notmatch 'CLRRuntimeHeaderRVA: 0x0\b' -or $audit -notmatch 'CLRRuntimeHeaderSize: 0x0\b') {
        throw "Tokenizer no-CRT PE contract failed`n$audit"
    }
    Write-Output "Built $binary; ARM64, Kernel32 only, no exception or CLR tables"
    if ($TestBlocks -or $TestPrompt) {
        $blockDir = Join-Path $BuildDir 'gemma-block'
        New-Item -ItemType Directory -Force -Path $blockDir | Out-Null
        $blockBinary = Join-Path $blockDir 'test-gemma-block.exe'
        & $compilerPath @flags experimental/snapdragon/tools/test-gemma-block.c `
            experimental/snapdragon/src/tools/gemma/gemma_block.c `
            experimental/snapdragon/src/tools/gemma/gemma_model.c `
            experimental/snapdragon/src/tools/gemma/gemma_artifact.c `
            experimental/snapdragon/src/tools/gemma/gemma_numeric.c src/shared/crypto/sha256.c -o $blockBinary
        if ($LASTEXITCODE -ne 0) { throw 'Block runner compilation failed' }
        $blockAudit = & $readObj --file-headers --coff-imports $blockBinary | Out-String
        $blockImports = @([regex]::Matches($blockAudit, '(?m)^\s*Name: (.+\.dll)\s*$'))
        if ($LASTEXITCODE -ne 0 -or $blockAudit -notmatch 'IMAGE_FILE_MACHINE_ARM64' -or
            $blockImports.Count -ne 1 -or $blockImports[0].Groups[1].Value.Trim() -ine 'KERNEL32.dll' -or
            $blockAudit -notmatch 'ExceptionTableRVA: 0x0\b' -or $blockAudit -notmatch 'CLRRuntimeHeaderRVA: 0x0\b') {
            throw 'Block runner PE contract failed'
        }
        Get-ChildItem -LiteralPath $BuildDir -File | Where-Object { $_.Extension -in '.dll', '.so', '.cat' } |
            Copy-Item -Destination $blockDir -Force
        if ($TestBlocks) {
            & "$PSScriptRoot/test-gemma-block.ps1" -Binary $blockBinary -Bits $BlockBits -Layers $BlockLayers -TestCleanup
        }
        if ($TestPrompt) {
            & "$PSScriptRoot/test-gemma-block.ps1" -Binary $blockBinary -PromptBucket $PromptBucket `
                -FixtureDir experimental/snapdragon/models/translategemma-4b-stage7 -RestorePrompt:$RestorePrompt
        }
    }
    if ($Test -or $TestNumerics) {
        $testArguments = @("$ModelDir/tokenizer.gta", "$ModelDir/tokenizer-fixtures.gta")
        if ($TestNumerics) {
            $manifest = Get-Content -LiteralPath "$NumericDir/manifest.json" -Raw -Encoding UTF8 | ConvertFrom-Json
            $contract = $manifest.execution_contract
            if ($manifest.schema_version -ne 2 -or $contract.global_rope.factor -ne 8 -or
                $contract.global_rope.theta -ne 1000000 -or $contract.global_rope.type -ne 'linear' -or
                $contract.local_rope.factor -ne 1 -or $contract.local_rope.theta -ne 10000 -or
                $contract.local_rope.type -ne 'default' -or $contract.residual.bf16_divisor -ne 1 -or
                $contract.residual.quantized_divisor -ne 32 -or $contract.residual.post_norm_gain_divisor -ne 32 -or
                $contract.residual.pre_norm_epsilon -ne 9.765625e-10 -or $contract.residual.storage -ne 'activation-dtype') {
                throw 'Numerical fixtures do not use the checkpoint RoPE/scaled-residual v2 contract'
            }
            $testArguments += "$NumericDir/numeric-scalars.gta"
        }
        & $binary @testArguments
        if ($LASTEXITCODE -ne 0) { throw 'Tokenizer tests failed' }
    }
} finally {
    if ($ExportNumerics) { $env:OPENBLAS_NUM_THREADS = $originalBlasThreads }
    Pop-Location
}