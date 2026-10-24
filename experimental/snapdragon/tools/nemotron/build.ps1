param(
    [string]$Compiler = 'clang',
    [string]$BuildDir = 'experimental\snapdragon\build\nemotron-asr',
    [string]$SdkArchive = 'experimental\snapdragon\data\v2.50.0.260828.zip',
    [switch]$StageRuntime,
    [switch]$Test,
    [switch]$TestLifecycle,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$isolated = [IO.Path]::GetFullPath((Join-Path $repo 'experimental\snapdragon\build\nemotron-asr'))
if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $repo $BuildDir }
$BuildDir = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\')
if ($BuildDir -ine $isolated) { throw "BuildDir must be the isolated child $isolated" }
function Assert-SafeBuild {
    $path = $BuildDir
    while ($path -and $path.Length -ge $repo.Length) {
        if (Test-Path -LiteralPath $path) {
            $item = Get-Item -LiteralPath $path -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse path rejected: $path" }
        }
        if ($path -ieq $repo) { break }
        $path = Split-Path -Parent $path
    }
    if (Test-Path -LiteralPath $BuildDir) {
        if (@(Get-ChildItem -LiteralPath $BuildDir -Recurse -Force |
            Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
            throw 'Reparse child rejected'
        }
    }
    foreach ($p in Get-Process) {
        try { $exe = $p.Path } catch { continue }
        if ($exe -and $exe.StartsWith($BuildDir + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Active process $($p.Id) uses isolated build"
        }
    }
}
Assert-SafeBuild
if ($Clean) {
    if (Test-Path -LiteralPath $BuildDir) { Remove-Item -LiteralPath $BuildDir -Recurse -Force }
    Write-Output "Cleaned only $BuildDir"
    return
}
Push-Location $repo
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
try {
    $cc = (Get-Command $Compiler -ErrorAction Stop).Source
    $toolDir = Split-Path -Parent $cc
    $dlltool = Join-Path $toolDir 'llvm-dlltool.exe'
    $readobj = Join-Path $toolDir 'llvm-readobj.exe'
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    $scratch = Join-Path $BuildDir 'compiler-scratch'
    New-Item -ItemType Directory -Force -Path $scratch | Out-Null
    $env:TEMP = $scratch
    $env:TMP = $scratch
    $src = 'experimental\snapdragon\src\apps\nemotron'
    & $dlltool -m arm64 -d "$src\imports\kernel32.def" -l "$BuildDir\libkernel32.a"
    if ($LASTEXITCODE) { throw 'Import library failed' }
    & $cc --target=aarch64-w64-windows-gnu -c 'src\arch\aarch64\windows\chkstk.S' -o "$BuildDir\chkstk.obj"
    if ($LASTEXITCODE) { throw 'Stack probe failed' }
    $flags = @('--target=aarch64-w64-windows-gnu', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
        '-Oz', '-march=armv8-a+crypto', '-DNEWOS_CRYPTO_SHA256_ENABLE_ARM_SHA=1',
        '-DNEWOS_RUNTIME_ALLOC_LOCK=0', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
        '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-ffunction-sections', '-fdata-sections', '-flto',
        '-Isrc\shared', '-Iexperimental\snapdragon\src\shared', "-I$src", '-nostdlib', '-fuse-ld=lld',
        '-Wl,-e,mainCRTStartup', '-Wl,-s', '-Wl,--gc-sections', '-Wl,--icf=safe', '-Wl,--no-insert-timestamp',
        '-Wl,/merge:.rdata=.text', '-Wl,--stack,1048576', "$BuildDir\chkstk.obj", "-L$BuildDir", '-lkernel32')
    $shared = @("$src\nemotron_reader.c", "$src\nemotron_windows.c", "$src\nemotron_tokenizer.c", 'src\shared\runtime\memory.c',
        'src\shared\runtime\string.c', 'src\shared\runtime\parse.c', 'src\shared\runtime\unicode_utf8.c',
        'src\shared\crypto\sha256.c', 'src\platform\windows\thread.c')
    foreach ($target in @('nemotron-probe', 'nemotron-prepare', 'test-reader', 'nemotron-frontend', 'nemotron-asr', 'test-asr', 'test-audio', 'test-live-cancel')) {
        $binary = Join-Path $BuildDir "$target.exe"
        $extra = @()
        $entry = "$src\nemotron.c"
        if ($target -eq 'nemotron-prepare') { $extra = @('-DNM_PREPARE=1') }
        if ($target -eq 'test-reader') { $entry = "$src\tests\test_reader.c" }
        if ($target -eq 'nemotron-frontend') {
            $entry = "$src\nemotron_frontend_main.c"
            $extra = @('-ffp-contract=off', "$src\nemotron_frontend.c", "$src\nemotron_wav.c", "$src\nemotron_runtime.c", 'src\shared\math.c')
        }
        if ($target -eq 'nemotron-asr' -or $target -eq 'test-asr') {
            $entry = if ($target -eq 'test-asr') { "$src\tests\test_asr.c" } else { "$src\nemotron_asr.c" }
            $extra = @('-ffp-contract=off', "$src\nemotron_frontend.c", "$src\nemotron_wav.c",
                "$src\nemotron_runtime.c", "$src\nemotron_rnnt.c", "$src\nemotron_asr_runtime.c",
                "$src\nemotron_audio.c", "$src\nemotron_capture.c", "$src\nemotron_live.c",
                "$src\fp32\nm_fastrpc_host.c", 'src\shared\runtime\concurrency.c', 'src\shared\math.c')
        }
        if ($target -eq 'test-audio') {
            $entry = "$src\tests\test_audio.c"
            $extra = @('-ffp-contract=off', "$src\nemotron_audio.c", "$src\nemotron_capture.c",
                       'src\shared\runtime\concurrency.c', 'src\shared\math.c')
        }
        if ($target -eq 'test-live-cancel') {
            $entry = "$src\tests\test_live_cancel.c"
            $extra = @('-ffp-contract=off', "$src\nemotron_audio.c", "$src\nemotron_capture.c",
                "$src\nemotron_live.c", "$src\nemotron_wav.c",
                'src\shared\runtime\concurrency.c', 'src\shared\math.c')
        }
        & $cc @flags @extra $entry @shared -o $binary
        if ($LASTEXITCODE) { throw "$target freestanding build failed" }
        $audit = & $readobj --file-headers --coff-imports $binary | Out-String
        if ($LASTEXITCODE) { throw "$target PE inspection failed" }
        $imports = @([regex]::Matches($audit, '(?m)^\s*Name: (.+\.dll)\s*$') |
            ForEach-Object { $_.Groups[1].Value.Trim() })
        if ($audit -notmatch 'IMAGE_FILE_MACHINE_ARM64' -or $imports.Count -ne 1 -or $imports[0] -ine 'KERNEL32.dll' -or
            $audit -notmatch 'ExceptionTableRVA: 0x0\b' -or $audit -notmatch 'ExceptionTableSize: 0x0\b' -or
            $audit -notmatch 'CLRRuntimeHeaderRVA: 0x0\b' -or $audit -notmatch 'CLRRuntimeHeaderSize: 0x0\b') {
            throw "$target no-CRT ARM64/import contract failed`n$audit"
        }
        Write-Output "Built and audited $binary (ARM64; Kernel32 only; no CRT/CLR/unwind)"
    }
    if ($StageRuntime -or $TestLifecycle) {
        Assert-SafeBuild
        & 'experimental\snapdragon\tools\shared\fetch-qnn-runtime.ps1' -BuildDir $BuildDir -SdkArchive $SdkArchive
        $hash = (Get-FileHash -LiteralPath "$BuildDir\QnnHtp.dll" -Algorithm SHA256).Hash
        if ($hash -ine '99ff3a16167451fc5ebe03781260b2b64c37e4042414999ebd4805d6a1683a23') {
            throw 'Staged runtime fingerprint does not match frozen contract'
        }
    }
    if ($Test -or $TestLifecycle) {
        & "$BuildDir\test-reader.exe"
        if ($LASTEXITCODE) { throw 'Native reader regression tests failed' }
        & "$BuildDir\test-asr.exe"
        if ($LASTEXITCODE) { throw 'Native ASR controller tests failed' }
        & "$BuildDir\test-audio.exe"
        if ($LASTEXITCODE) { throw 'Native audio/capture/endpoint tests failed' }
    }
    if ($TestLifecycle) {
        $smoke = Join-Path $BuildDir ('lifecycle-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $smoke | Out-Null
        function Assert-TimingOnlyResult([string]$Path) {
            $data = [IO.File]::ReadAllBytes($Path)
            $tensors = [BitConverter]::ToUInt32($data, 52)
            $steps = [BitConverter]::ToUInt32($data, 60)
            $frequency = [BitConverter]::ToUInt64($data, 200)
            $stride = 64 + $tensors * 32
            $metadata = 64 + $tensors * 160 + $steps * $stride
            if (-not $frequency -or -not $steps -or $data.Length -ne 256 + [math]::Ceiling($metadata / 16) * 16 -or
                [BitConverter]::ToUInt64($data, 224) -ne 0) {
                throw 'Benchmark result contains snapshots or invalid clock/profile metadata'
            }
            [double]$total = 0
            for ($step = 0; $step -lt $steps; ++$step) {
                $offset = 256 + 64 + $tensors * 160 + $step * $stride
                $wall = [BitConverter]::ToUInt64($data, $offset + 32)
                if (-not $wall -or [BitConverter]::ToUInt64($data, $offset + 40) -ne 0 -or
                    [BitConverter]::ToUInt64($data, $offset + 48) -ne 0) {
                    throw 'Benchmark performed correctness comparisons or omitted timings'
                }
                $total += $wall / $frequency
                for ($tensor = 0; $tensor -lt $tensors; ++$tensor) {
                    $cell = $offset + 64 + $tensor * 32
                    if ([BitConverter]::ToUInt32($data, $cell) -ne 2) { throw 'Benchmark cell is not ZERO' }
                    for ($byte = 4; $byte -lt 32; ++$byte) {
                        if ($data[$cell + $byte]) { throw 'Benchmark cell has snapshot fields' }
                    }
                }
            }
            $mean = $total / $steps
            if ($mean -le 0 -or [double]::IsNaN($mean) -or [double]::IsInfinity($mean)) {
                throw 'Benchmark mean is not finite and positive'
            }
            Write-Output "Timing-only benchmark mean seconds $mean (not a correctness result)"
        }
        foreach ($dtype in @('float32', 'float16', 'quant16')) {
            $prefix = Join-Path $smoke $dtype
            $emit = if ($dtype -eq 'quant16') { '--emit-smoke-quant' } elseif ($dtype -eq 'float16') { '--emit-smoke-half' } else { '--emit-smoke' }
            $emitPipeline = if ($dtype -eq 'quant16') { '--emit-pipeline-quant' } elseif ($dtype -eq 'float16') { '--emit-pipeline-half' } else { '--emit-pipeline' }
            & "$BuildDir\test-reader.exe" $emit "$prefix.nmg" "$prefix.nmf"
            if ($LASTEXITCODE) { throw 'Smoke artifact emission failed' }
            & "$BuildDir\nemotron-prepare.exe" prepare "$prefix.nmg" "$prefix.nmc"
            if ($LASTEXITCODE) { throw 'HTP Add preparation failed' }
            & "$BuildDir\nemotron-probe.exe" run "$prefix.nmc" "$prefix.nmf" "$prefix.nmr"
            if ($LASTEXITCODE) { throw 'HTP Add restore/execute/comparison failed' }
            & "$BuildDir\nemotron-probe.exe" validate "$prefix.nmr"
            if ($LASTEXITCODE) { throw 'HTP Add result validation failed' }
            & "$BuildDir\test-reader.exe" --mismatch-fixture "$prefix.nmf" "$prefix-mismatch.nmf"
            if ($LASTEXITCODE) { throw 'Negative fixture emission failed' }
            & "$BuildDir\nemotron-probe.exe" run "$prefix.nmc" "$prefix-mismatch.nmf" "$prefix-mismatch.nmr"
            if ($LASTEXITCODE -ne 1) { throw 'Incorrect outputs did not return the comparison failure code' }
            & "$BuildDir\nemotron-probe.exe" validate "$prefix-mismatch.nmr"
            if ($LASTEXITCODE) { throw 'Diagnostic result validation failed' }
            $diagnostic = [IO.File]::ReadAllBytes("$prefix-mismatch.nmr")
            if ([BitConverter]::ToUInt32($diagnostic, 256 + 384 + 44) -ne 1) {
                throw 'Negative gate did not persist its output violation'
            }
            & "$BuildDir\test-reader.exe" $emitPipeline "$prefix-pipeline.nmf"
            if ($LASTEXITCODE) { throw 'Pipeline fixture emission failed' }
            & "$BuildDir\nemotron-probe.exe" pipeline "$prefix.nmc" "$prefix.nmc" "$prefix.nmc" "$prefix-pipeline.nmf" "$prefix-pipeline.nmr"
            if ($LASTEXITCODE) { throw 'Three-context HTP carry pipeline failed' }
            & "$BuildDir\nemotron-probe.exe" validate "$prefix-pipeline.nmr"
            if ($LASTEXITCODE) { throw 'Pipeline result validation failed' }
            & "$BuildDir\nemotron-probe.exe" run-benchmark "$prefix.nmc" "$prefix-mismatch.nmf" "$prefix-benchmark.nmr"
            if ($LASTEXITCODE) { throw 'Timing-only single-graph CLI failed' }
            & "$BuildDir\nemotron-probe.exe" validate "$prefix-benchmark.nmr"
            if ($LASTEXITCODE) { throw 'Timing-only result validation failed' }
            Assert-TimingOnlyResult "$prefix-benchmark.nmr"
            & "$BuildDir\nemotron-probe.exe" pipeline-benchmark "$prefix.nmc" "$prefix.nmc" "$prefix.nmc" "$prefix-pipeline.nmf" "$prefix-pipeline-benchmark.nmr"
            if ($LASTEXITCODE) { throw 'Timing-only carry pipeline CLI failed' }
            & "$BuildDir\nemotron-probe.exe" validate "$prefix-pipeline-benchmark.nmr"
            if ($LASTEXITCODE) { throw 'Timing-only pipeline result validation failed' }
            Assert-TimingOnlyResult "$prefix-pipeline-benchmark.nmr"
            if ($dtype -eq 'quant16') {
                & "$BuildDir\test-reader.exe" --emit-smoke-quant-zero "$prefix-zero.nmg" "$prefix-zero.nmf"
                if ($LASTEXITCODE) { throw 'Quantized ZERO fixture emission failed' }
                & "$BuildDir\nemotron-probe.exe" run "$prefix.nmc" "$prefix-zero.nmf" "$prefix-zero.nmr"
                if ($LASTEXITCODE) { throw 'Quantized ZERO raw zero-point initialization failed' }
                & "$BuildDir\nemotron-probe.exe" validate "$prefix-zero.nmr"
                if ($LASTEXITCODE) { throw 'Quantized ZERO result validation failed' }
                $quantized = [IO.File]::ReadAllBytes("$prefix.nmr")
                $zero = [IO.File]::ReadAllBytes("$prefix-zero.nmr")
                if ([BitConverter]::ToUInt32($quantized, 8) -ne 2 -or
                    [BitConverter]::ToUInt32($quantized, 48) -ne 32 -or
                    [BitConverter]::ToUInt64($quantized, 256 + 480 + 16) -ne 2 -or
                    [BitConverter]::ToUInt16($quantized, 256 + 512) -ne 45056 -or
                    [BitConverter]::ToUInt16($zero, 256 + 512) -ne 40960) {
                    throw 'Quantized result did not retain exact raw U16 values'
                }
            }
            Write-Output "Tiny HTP $dtype lifecycle and carry pipeline passed"
        }
        Write-Output "Lifecycle artifacts: $smoke"
    }
} finally {
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
    Pop-Location
}
