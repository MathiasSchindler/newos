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
    [switch]$Translate,
    [switch]$Gui,
    [switch]$TestDocuments,
    [switch]$ProfileTranslate,
    [switch]$BuildDecode,
    [switch]$BuildBundle,
    [switch]$BuildPartitions,
    [switch]$PrepareBindings,
    [ValidateSet(4, 8)][int]$WeightBits = 4,
    [ValidateRange(1, 3600)][int]$BuildTimeoutSeconds = 600,
    [switch]$TestEnvelope,
    [switch]$ScalarHash,
    [switch]$RowMajorProjections,
    [switch]$FullyConnected,
    [switch]$NpuSelection,
    [switch]$TestSelection,
    [switch]$BuildSelection,
    [ValidateSet(512, 1024, 2048)][int]$PromptBucket = 512,
    [ValidateSet(4, 8)][int[]]$BlockBits = @(8, 4),
    [ValidateSet(0, 5)][int[]]$BlockLayers = @(0, 5),
    [string]$NumericDir = 'experimental/snapdragon/models/translategemma-4b-stage5-v2',
    [string]$Python = 'experimental/snapdragon/build/calibration-venv/Scripts/python.exe'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
$originalBlasThreads = $env:OPENBLAS_NUM_THREADS
Push-Location $repoRoot
try {
    if ($ExportNumerics) {
        $env:OPENBLAS_NUM_THREADS = '4'
        & $Python experimental/snapdragon/tools/translate/export-translategemma.py `
            --numerical-reference --replace --output $NumericDir
        if ($LASTEXITCODE -ne 0) { throw 'Numerical reference export failed' }
    }
    if ($ExportReference) {
        & $Python experimental/snapdragon/tools/translate/export-translategemma.py `
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
        '-march=armv8-a+crypto', '-DNEWOS_CRYPTO_SHA256_ENABLE_ARM_SHA=1',
        '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables',
        '-fno-asynchronous-unwind-tables', '-ffunction-sections', '-fdata-sections', '-flto',
        '-Isrc/shared', '-Iexperimental/snapdragon/src/shared', '-Iexperimental/snapdragon/src/apps/translate',
        '-nostdlib', '-fuse-ld=lld', '-Wl,-e,mainCRTStartup', '-Wl,-s', '-Wl,--gc-sections',
        '-Wl,--icf=safe', '-Wl,--no-insert-timestamp', '-Wl,/merge:.rdata=.text',
        '-Wl,--stack,1048576', "$BuildDir/gemma-chkstk.obj", "-L$BuildDir", '-lkernel32'
    )
    $binary = "$BuildDir/test-gemma-tokenizer.exe"
    if ($ScalarHash) { $flags += '-DNEWOS_CRYPTO_SHA256_DISABLE_ARM_SHA=1' }
    if ($RowMajorProjections) { $flags += '-DGEMMA_ROW_MAJOR_PROJECTIONS=1' }
    if ($FullyConnected) { $flags += '-DGEMMA_ROW_MAJOR_PROJECTIONS=1'; $flags += '-DGEMMA_FULLY_CONNECTED=1' }
    if ($NpuSelection) { $flags += '-DGEMMA_NPU_SELECTION=1' }
    & $compilerPath @flags experimental/snapdragon/src/apps/translate/tests/test-gemma-tokenizer.c `
        experimental/snapdragon/src/apps/translate/gemma_model.c `
        experimental/snapdragon/src/apps/translate/gemma_artifact.c `
        experimental/snapdragon/src/apps/translate/gemma_numeric.c `
        experimental/snapdragon/src/apps/translate/tests/test-gemma-numeric.c `
        experimental/snapdragon/src/apps/translate/gemma_tokenizer.c src/shared/crypto/sha256.c -o $binary
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
    if (($Gui -and ($Translate -or $ProfileTranslate -or $TestDocuments)) -or ($TestDocuments -and ($Translate -or $ProfileTranslate))) { throw 'Build GUI, CLI and document test targets separately' }
    if ($Translate -or $ProfileTranslate -or $Gui -or $TestDocuments) {
        $translateName = if ($TestDocuments) { 'test-translate-document.exe' } elseif ($Gui) { 'translate-gui.exe' } elseif ($ProfileTranslate) { 'translate-profile.exe' } else { 'translate.exe' }
        $translateBinary = Join-Path $BuildDir $translateName
        $translateFlags = @('-O2')
        if ($ProfileTranslate) { $translateFlags += '-DGEMMA_TRANSLATE_PROFILE' }
        $translateSource = 'experimental/snapdragon/src/apps/translate/translate.c'
        if ($TestDocuments) { $translateSource = 'experimental/snapdragon/src/apps/translate/tests/test-translate-document.c' }
        if ($Gui) {
            $translateSource = 'experimental/snapdragon/src/apps/translate/translate_gui.c'
            foreach ($library in 'user32','gdi32') {
                & $dllTool -m arm64 -d ('experimental/snapdragon/src/shared/imports/{0}.def' -f $library) -l ('{0}/lib{1}.a' -f $BuildDir,$library)
                if ($LASTEXITCODE -ne 0) { throw 'GUI import library creation failed' }
            }
            $translateFlags += @('-luser32', '-lgdi32', '-Wl,--subsystem,windows')
        }
        & $compilerPath @flags @translateFlags $translateSource `
            experimental/snapdragon/src/apps/translate/gemma_block.c `
            experimental/snapdragon/src/apps/translate/gemma_model.c `
            experimental/snapdragon/src/apps/translate/gemma_artifact.c `
            experimental/snapdragon/src/apps/translate/gemma_numeric.c `
            experimental/snapdragon/src/apps/translate/gemma_tokenizer.c src/shared/crypto/sha256.c -o $translateBinary
        if ($LASTEXITCODE -ne 0) { throw 'Translator compilation failed' }
        $translateAudit = & $readObj --file-headers --coff-imports $translateBinary | Out-String
        $translateImports = @([regex]::Matches($translateAudit, '(?m)^\s*Name: (.+\.dll)\s*$'))
        $expectedImports = if ($Gui) { @('KERNEL32.dll','USER32.dll','GDI32.dll') } else { @('KERNEL32.dll') }
        $actualImports = @($translateImports | ForEach-Object { $_.Groups[1].Value.Trim() })
        if ($LASTEXITCODE -ne 0 -or $translateAudit -notmatch 'IMAGE_FILE_MACHINE_ARM64' -or
            @(Compare-Object $expectedImports $actualImports).Count -ne 0 -or
            ($Gui -and $translateAudit -notmatch 'IMAGE_SUBSYSTEM_WINDOWS_GUI') -or
            $translateAudit -notmatch 'ExceptionTableRVA: 0x0\b' -or $translateAudit -notmatch 'ExceptionTableSize: 0x0\b' -or
            $translateAudit -notmatch 'CLRRuntimeHeaderRVA: 0x0\b' -or $translateAudit -notmatch 'CLRRuntimeHeaderSize: 0x0\b') {
            throw 'Translator no-CRT PE contract failed'
        }
        Write-Output ('Built {0}; ARM64, imports: {1}; no exception or CLR tables' -f $translateBinary,($actualImports -join ', '))
        if ($TestDocuments) {
            & $translateBinary
            if ($LASTEXITCODE -ne 0) { throw 'Native document tests failed' }
        }
    }
    if ($TestBlocks -or $TestPrompt -or $BuildDecode -or $BuildBundle -or $BuildPartitions -or $PrepareBindings -or $TestEnvelope -or $TestSelection -or $BuildSelection) {
        $blockDir = Join-Path $BuildDir 'gemma-block'
        New-Item -ItemType Directory -Force -Path $blockDir | Out-Null
        $blockBinary = Join-Path $blockDir 'test-gemma-block.exe'
        & $compilerPath @flags experimental/snapdragon/src/apps/translate/gemma_runner.c `
            experimental/snapdragon/src/apps/translate/gemma_block.c `
            experimental/snapdragon/src/apps/translate/gemma_model.c `
            experimental/snapdragon/src/apps/translate/gemma_artifact.c `
            experimental/snapdragon/src/apps/translate/gemma_numeric.c src/shared/crypto/sha256.c -o $blockBinary
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
        if ($PrepareBindings) {
            if ($WeightBits -eq 8 -and [IO.Path]::GetFullPath($BuildDir).TrimEnd('\','/') -eq [IO.Path]::GetFullPath('experimental/snapdragon/build')) {
                throw 'Prepare a W8 candidate in an isolated -BuildDir before deployment'
            }
            $bindingPath = Join-Path $blockDir 'prompt-512.gmb'
            if (Test-Path -Path ($bindingPath + '.*bundle.context')) { throw 'Existing bundle: do not replace its binding during preparation' }
            & "$PSScriptRoot/test-gemma-block.ps1" -Binary $blockBinary -PromptBucket 512 -Bits $WeightBits -PrepareOnly `
                -FixtureDir experimental/snapdragon/models/translategemma-4b-stage7
        }
        if ($TestSelection) {
            & $blockBinary (Join-Path $blockDir 'prompt-512.gmb') selection-regression
            if ($LASTEXITCODE -ne 0) { throw 'NPU selection regression failed' }
        }
        if ($BuildSelection) {
            & $blockBinary (Join-Path $blockDir 'prompt-512.gmb') build-selection
            if ($LASTEXITCODE -ne 0) { throw 'NPU selection cache build failed' }
        }
        if ($BuildDecode) {
            & $blockBinary (Join-Path $blockDir 'prompt-512.gmb') build-decode-512
            if ($LASTEXITCODE -ne 0) { throw 'Decode context build failed' }
        }
        if ($BuildBundle -or $BuildPartitions) {
            if ($BuildBundle -and $BuildPartitions) { throw 'Choose monolithic or partitioned bundle construction' }
            $start = [Diagnostics.ProcessStartInfo]::new()
            $start.FileName = (Resolve-Path $blockBinary).Path
            $mode = if ($BuildPartitions) { 'build-partitions-512' } else { 'build-bundle-512' }
            $start.Arguments = '"' + (Resolve-Path (Join-Path $blockDir 'prompt-512.gmb')).Path + '" ' + $mode
            $start.WorkingDirectory = (Resolve-Path $blockDir).Path
            $start.UseShellExecute = $false
            $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
            $process = [Diagnostics.Process]::new(); $process.StartInfo = $start
            $output = [IO.File]::Create((Join-Path $start.WorkingDirectory ($mode + '.stdout.txt')))
            $errors = [IO.File]::Create((Join-Path $start.WorkingDirectory ($mode + '.stderr.txt')))
            try {
                Write-Output ('Building/restoring shared bundle, timeout {0}s; logs in {1}' -f $BuildTimeoutSeconds,$start.WorkingDirectory)
                if (-not $process.Start()) { throw 'Cannot launch shared bundle builder' }
                $stdout = $process.StandardOutput.BaseStream.CopyToAsync($output)
                $stderr = $process.StandardError.BaseStream.CopyToAsync($errors)
                $finished = $process.WaitForExit($BuildTimeoutSeconds * 1000)
                if (-not $finished) { $process.Kill(); $process.WaitForExit() }
                $null = $stdout.GetAwaiter().GetResult(); $null = $stderr.GetAwaiter().GetResult()
                if (-not $finished) { throw 'Shared-weight bundle build timed out; candidate not deployed' }
                if ($process.ExitCode -ne 0) { throw ('Shared-weight bundle build failed: ' + $process.ExitCode) }
                Write-Output 'PASS shared-weight bundle build and restore'
            } finally { $output.Dispose(); $errors.Dispose(); $process.Dispose() }
        }
        if ($TestEnvelope) {
            & $blockBinary (Join-Path $blockDir 'prompt-512.gmb') envelope-regression
            if ($LASTEXITCODE -ne 0) { throw 'Prompt/decode envelope tests failed' }
        }
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