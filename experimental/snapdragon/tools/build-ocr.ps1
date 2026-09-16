param(
    [string]$Compiler = 'clang',
    [string]$BuildDir = 'experimental/snapdragon/build/ocr',
    [string]$ModelDir = 'experimental/snapdragon/models/glm-ocr',
    [string]$TokenizerDir = 'experimental/snapdragon/models/glm-ocr-tokenizer-v2',
    [string]$ImageDir = 'experimental/snapdragon/models/glm-ocr-images-v2',
    [switch]$Download,
    [switch]$Verify,
    [switch]$Test,
    [switch]$TestTokenizer,
    [switch]$TestImages
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path

function Get-GitBlobHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hash = [Security.Cryptography.SHA1]::Create()
    try {
        $prefix = [Text.Encoding]::ASCII.GetBytes(('blob {0}' -f $stream.Length) + [char]0)
        $null = $hash.TransformBlock($prefix, 0, $prefix.Length, $prefix, 0)
        $buffer = New-Object byte[] (1024 * 1024)
        while (($received = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $null = $hash.TransformBlock($buffer, 0, $received, $buffer, 0)
        }
        $null = $hash.TransformFinalBlock((New-Object byte[] 0), 0, 0)
        return ([BitConverter]::ToString($hash.Hash)).Replace('-', '').ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $hash.Dispose()
    }
}

function Read-Exact($Stream, [byte[]]$Buffer) {
    $offset = 0
    while ($offset -lt $Buffer.Length) {
        $received = $Stream.Read($Buffer, $offset, $Buffer.Length - $offset)
        if ($received -eq 0) { throw 'Truncated safetensors header' }
        $offset += $received
    }
}

function Get-TensorInventory([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $prefix = New-Object byte[] 8
        Read-Exact $stream $prefix
        $headerSize = [BitConverter]::ToUInt64($prefix, 0)
        if ($headerSize -lt 2 -or $headerSize -gt 16777216 -or $headerSize -gt $stream.Length - 8) {
            throw 'Invalid safetensors header length'
        }
        $header = New-Object byte[] ([int]$headerSize)
        Read-Exact $stream $header
        $utf8 = New-Object Text.UTF8Encoding($false, $true)
        $jsonText = $utf8.GetString($header)
        if (-not $jsonText.StartsWith('{')) { throw 'Safetensors header must be a JSON object' }
        $metadata = ConvertFrom-Json -InputObject $jsonText
        $dataBytes = $stream.Length - 8 - [long]$headerSize
        $tensors = @()
        $widths = @{ BF16 = 2; F16 = 2; F32 = 4; I64 = 8; I32 = 4; I16 = 2; I8 = 1; U8 = 1; BOOL = 1 }
        foreach ($property in $metadata.PSObject.Properties) {
            if ($property.Name -eq '__metadata__') { continue }
            $tensor = $property.Value
            if (-not $widths.ContainsKey([string]$tensor.dtype) -or $null -eq $tensor.shape -or
                $tensor.shape -isnot [array] -or $tensor.data_offsets -isnot [array] -or $tensor.data_offsets.Count -ne 2) {
                throw ('Invalid tensor descriptor: {0}' -f $property.Name)
            }
            [decimal]$elements = 1
            foreach ($dimension in $tensor.shape) {
                if (($dimension -isnot [int] -and $dimension -isnot [long]) -or $dimension -le 0) {
                    throw ('Invalid tensor dimension: {0}' -f $property.Name)
                }
                $elements *= [decimal]$dimension
                if ($elements -gt [long]::MaxValue) { throw 'Tensor element count overflow' }
            }
            foreach ($offset in $tensor.data_offsets) {
                if (($offset -isnot [int] -and $offset -isnot [long]) -or $offset -lt 0 -or $offset -gt $dataBytes) {
                    throw 'Invalid tensor offset'
                }
            }
            [long]$begin = $tensor.data_offsets[0]
            [long]$end = $tensor.data_offsets[1]
            if ($end -lt $begin -or [decimal]($end - $begin) -ne $elements * $widths[[string]$tensor.dtype]) {
                throw ('Tensor shape/byte mismatch: {0}' -f $property.Name)
            }
            $tensors += [ordered]@{
                name = $property.Name; dtype = $tensor.dtype; shape = $tensor.shape
                elements = [long]$elements; begin = $begin; end = $end
            }
        }
        if ($tensors.Count -eq 0) { throw 'Empty tensor inventory' }
        [long]$cursor = 0
        [long]$totalElements = 0
        foreach ($tensor in ($tensors | Sort-Object { $_.begin })) {
            if ($tensor.begin -ne $cursor) { throw 'Tensor payload contains a gap or overlap' }
            $cursor = $tensor.end
            $totalElements += $tensor.elements
        }
        if ($cursor -ne $dataBytes) { throw 'Unclaimed bytes in tensor payload' }
        return [ordered]@{
            schema_version = 1; tensor_count = $tensors.Count; stored_elements = $totalElements
            header_bytes = $headerSize; payload_bytes = $dataBytes; tensors = $tensors
        }
    } finally { $stream.Dispose() }
}

function Assert-Native([string[]]$Arguments, [int]$Expected = 0) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $messages = & $binary @Arguments 2>&1
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previous }
    if ($code -ne $Expected) { throw ('Native OCR exit {0}, expected {1}: {2}' -f $code,$Expected,($messages -join ' ')) }
}

function Test-Ocr {
    $scratch = Join-Path $repoRoot ('tests/tmp/ocr-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $scratch | Out-Null
    $checks = 0
    try {
        $path = Join-Path $scratch ('unicode ' + [char]0x00e4 + '.bin')
        foreach ($length in @(0,1,55,56,63,64,65,127,128,129,1048575,1048576,1048577)) {
            $bytes = New-Object byte[] $length
            for ($index = 0; $index -lt $length; ++$index) { $bytes[$index] = [byte](($index * 17 + 31) % 256) }
            [IO.File]::WriteAllBytes($path, $bytes)
            $digest = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            Assert-Native @('--verify', $path, $digest)
            Assert-Native @('--verify', $path, ('0' * 64)) 1
            $checks += 2
        }
        Assert-Native @('--verify', (Join-Path $scratch 'missing'), ('0' * 64)) 1
        Assert-Native @('--verify', $path, ('g' * 64)) 2
        Assert-Native @('--verify', $path, 'abcd') 2
        Assert-Native @('--unknown') 2
        $checks += 4
        foreach ($size in @([UInt64]0,[UInt64]1,[UInt64]16777217,[UInt64]::MaxValue)) {
            [IO.File]::WriteAllBytes($path, [BitConverter]::GetBytes($size))
            $digest = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            Assert-Native @('--weights', $path, $digest) 1
            $checks++
        }
        $cases = @(
            @{ json = '{"weight":{"dtype":"BF16","shape":[2,2],"data_offsets":[0,8]}}'; valid = $true },
            @{ json = '{"weight":{"dtype":"BF16","shape":[2,3],"data_offsets":[0,8]}}'; valid = $false },
            @{ json = '{"weight":{"dtype":"BF16","shape":[-2,2],"data_offsets":[0,8]}}'; valid = $false },
            @{ json = '{"weight":{"dtype":"BF16","shape":[2.5],"data_offsets":[0,8]}}'; valid = $false },
            @{ json = '{"weight":{"dtype":"F64","shape":[1],"data_offsets":[0,8]}}'; valid = $false },
            @{ json = '{"weight":{"dtype":"BF16","shape":[4],"data_offsets":[1,9]}}'; valid = $false },
            @{ json = '{"weight":{"dtype":"BF16","shape":[3],"data_offsets":[2,8]}}'; valid = $false },
            @{ json = '{"weight":{"dtype":"BF16","shape":[3],"data_offsets":[0,6]}}'; valid = $false },
            @{ json = '{"one":{"dtype":"BF16","shape":[3],"data_offsets":[0,6]},"two":{"dtype":"BF16","shape":[2],"data_offsets":[4,8]}}'; valid = $false },
            @{ json = '{}'; valid = $false },
            @{ json = '{invalid}'; valid = $false }
        )
        foreach ($case in $cases) {
            $header = [Text.Encoding]::UTF8.GetBytes($case.json)
            [byte[]]$artifact = [BitConverter]::GetBytes([UInt64]$header.Length) + $header + (New-Object byte[] 8)
            [IO.File]::WriteAllBytes($path, $artifact)
            $accepted = $false
            try { $null = Get-TensorInventory $path; $accepted = $true } catch { }
            if ($accepted -ne $case.valid) { throw ('Tensor regression failed: {0}' -f $case.json) }
            if ($accepted) {
                $digest = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
                Assert-Native @('--weights', $path, $digest)
                $checks++
            }
            $checks++
        }
        Write-Output ('PASS OCR regression: {0} checks; hash boundaries, Unicode paths, failures, tensor ranges' -f $checks)
    } finally { Remove-Item -LiteralPath $scratch -Recurse -Force }
}

function Test-SourceFile($Entry, [string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -ne $Entry.size) {
        throw ('Missing or wrong-sized source: {0}' -f $Entry.name)
    }
    if ($Entry.git_blob_sha1) {
        if ((Get-GitBlobHash $Path) -ne $Entry.git_blob_sha1) { throw ('Git blob mismatch: {0}' -f $Entry.name) }
        $digest = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    } else { $digest = $Entry.sha256 }
    $mode = if ($Entry.name -eq 'model.safetensors') { '--weights' } else { '--verify' }
    Assert-Native @($mode, $Path, $digest)
    return $digest
}

function Stage-Model {
    $catalogPath = Join-Path $PSScriptRoot 'glm-ocr-model.json'
    $catalog = Get-Content -LiteralPath $catalogPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($catalog.schema_version -ne 1 -or $catalog.repository -ne 'zai-org/GLM-OCR' -or
        $catalog.revision -notmatch '^[a-f0-9]{40}$' -or $catalog.files.Count -ne 9) { throw 'Invalid OCR source catalog' }
    $destination = [IO.Path]::GetFullPath((Join-Path $repoRoot $ModelDir))
    if ([IO.Path]::IsPathRooted($ModelDir)) { $destination = [IO.Path]::GetFullPath($ModelDir) }
    $exists = Test-Path -LiteralPath $destination
    if (-not $exists -and -not $Download) { throw 'Model is absent; run build-ocr.ps1 -Download first' }
    $stage = if ($exists) { $destination } else { "$destination.partial" }
    $guard = $null
    try {
        if ($Download) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
            $guard = [IO.FileStream]::new("$destination.download.lock", [IO.FileMode]::OpenOrCreate,
                [IO.FileAccess]::ReadWrite, [IO.FileShare]::None, 4096, [IO.FileOptions]::DeleteOnClose)
            New-Item -ItemType Directory -Force -Path $stage | Out-Null
        }
        $records = @()
        foreach ($entry in $catalog.files) {
            if ($entry.name -notmatch '^[A-Za-z0-9_.-]+$') { throw 'Unsafe catalog filename' }
            $path = Join-Path $stage $entry.name
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                if ($exists -or -not $Download) { throw ('Incomplete existing model; refusing to overwrite: {0}' -f $path) }
                $partial = "$path.partial"
                if (-not (Test-Path -LiteralPath $partial) -or (Get-Item -LiteralPath $partial).Length -lt $entry.size) {
                    Write-Output ('Downloading {0} ({1} bytes), pinned revision {2}' -f $entry.name,$entry.size,$catalog.revision)
                    $url = 'https://huggingface.co/{0}/resolve/{1}/{2}?download=true' -f $catalog.repository,$catalog.revision,$entry.name
                    & curl.exe --fail --location --silent --show-error --retry 3 --connect-timeout 30 --max-time 1800 --continue-at - --output $partial $url
                    if ($LASTEXITCODE -ne 0) { throw 'Model download failed; partial bytes retained for retry' }
                }
                $digest = Test-SourceFile $entry $partial
                Move-Item -LiteralPath $partial -Destination $path
            } else { $digest = Test-SourceFile $entry $path }
            $records += [ordered]@{ name = $entry.name; size = $entry.size; sha256 = $digest; git_blob_sha1 = $entry.git_blob_sha1 }
            Write-Output ('Verified {0}' -f $entry.name)
        }
        $config = Get-Content -LiteralPath (Join-Path $stage 'config.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($config.model_type -ne 'glm_ocr' -or $config.text_config.num_hidden_layers -ne 16 -or
            $config.text_config.hidden_size -ne 1536 -or $config.text_config.vocab_size -ne 59392 -or
            $config.vision_config.depth -ne 24 -or $config.vision_config.hidden_size -ne 1024) {
            throw 'Unexpected GLM-OCR architecture'
        }
        $inventory = Get-TensorInventory (Join-Path $stage 'model.safetensors')
        if ($inventory.tensor_count -ne 526 -or $inventory.stored_elements -ne 1325258240 -or
            $inventory.payload_bytes -ne 2650516480 -or
            @($inventory.tensors | Where-Object { $_.dtype -ne 'BF16' }).Count -ne 0) {
            throw 'Pinned GLM-OCR tensor inventory differs from Stage 1 contract'
        }
        $inventory.repository = $catalog.repository
        $inventory.revision = $catalog.revision
        $inventory.weights_sha256 = ($records | Where-Object { $_.name -eq 'model.safetensors' }).sha256
        $inventory.inference_ready = $false
        $inventory.npu_validated = $false
        $lock = [ordered]@{
            schema_version = 1; repository = $catalog.repository; revision = $catalog.revision
            license = $catalog.license; catalog_sha256 = (Get-FileHash -LiteralPath $catalogPath -Algorithm SHA256).Hash.ToLowerInvariant()
            files = $records
        }
        foreach ($item in @(@{ name = 'tensor-audit.json'; value = $inventory }, @{ name = 'source-lock.json'; value = $lock })) {
            $output = Join-Path $stage $item.name
            [IO.File]::WriteAllText("$output.partial", ($item.value | ConvertTo-Json -Depth 15) + "`n", (New-Object Text.UTF8Encoding($false)))
            Move-Item -LiteralPath "$output.partial" -Destination $output -Force
        }
        if (-not $exists) { Move-Item -LiteralPath $stage -Destination $destination }
        Write-Output ('PASS GLM-OCR source: {0} tensors, {1} stored elements, {2} payload bytes' -f $inventory.tensor_count,$inventory.stored_elements,$inventory.payload_bytes)
        $inventory.tensors | Group-Object { $_.dtype } | Select-Object Name,Count | Format-Table
        Write-Output ('Model: {0}' -f $destination)
        Write-Output 'Stage 1 only: source integrity and geometry verified; no OCR inference or QNN execution yet.'
    } finally { if ($null -ne $guard) { $guard.Dispose() } }
}

Push-Location $repoRoot
try {
    $compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
    $compilerDirectory = Split-Path -Parent $compilerPath
    $dllTool = Join-Path $compilerDirectory 'llvm-dlltool.exe'
    $readObj = Join-Path $compilerDirectory 'llvm-readobj.exe'
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    & $dllTool -m arm64 -d experimental/snapdragon/src/shared/imports/kernel32.def -l "$BuildDir/libkernel32.a"
    if ($LASTEXITCODE -ne 0) { throw 'OCR import library creation failed' }
    & $compilerPath --target=aarch64-w64-windows-gnu -c src/arch/aarch64/windows/chkstk.S -o "$BuildDir/chkstk.obj"
    if ($LASTEXITCODE -ne 0) { throw 'OCR stack probe compilation failed' }
    $flags = @(
        '--target=aarch64-w64-windows-gnu', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O2',
        '-march=armv8-a+crypto', '-DNEWOS_CRYPTO_SHA256_ENABLE_ARM_SHA=1',
        '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables',
        '-fno-asynchronous-unwind-tables', '-ffunction-sections', '-fdata-sections', '-flto',
        '-Isrc/shared', '-nostdlib', '-fuse-ld=lld', '-Wl,-e,mainCRTStartup', '-Wl,-s',
        '-Wl,--gc-sections', '-Wl,--icf=safe', '-Wl,--no-insert-timestamp', '-Wl,/merge:.rdata=.text',
        '-Wl,--stack,1048576', "$BuildDir/chkstk.obj", "-L$BuildDir", '-lkernel32'
    )
    $binary = Join-Path $BuildDir 'ocr-model.exe'
    $sources = @('experimental/snapdragon/src/tools/ocr/ocr_model.c', 'src/shared/crypto/sha256.c')
    if ($TestTokenizer -or $TestImages) {
        $binary = Join-Path $BuildDir 'ocr-tokenizer-test.exe'
        $flags += '-DOCR_TOKENIZER_TEST'
        $sources += @('experimental/snapdragon/src/tools/ocr/ocr_tokenizer.c', 'experimental/snapdragon/src/tools/ocr/ocr_tokenizer_test.c')
    }
    if ($TestImages) {
        $binary = Join-Path $BuildDir 'ocr-image-test.exe'
        $flags += @('-DOCR_IMAGE_TEST', '-fno-math-errno', '-ffp-contract=off')
        $sources += 'experimental/snapdragon/src/tools/ocr/ocr_image.c'
    }
    & $compilerPath @flags @sources -o $binary
    if ($LASTEXITCODE -ne 0) { throw 'OCR native build failed' }
    $audit = & $readObj --file-headers --coff-imports $binary | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'OCR PE inspection failed' }
    $imports = @([regex]::Matches($audit, '(?m)^\s*Name: (.+\.dll)\s*$') | ForEach-Object { $_.Groups[1].Value.Trim() })
    if ($audit -notmatch 'IMAGE_FILE_MACHINE_ARM64' -or $imports.Count -ne 1 -or $imports[0] -ine 'KERNEL32.dll' -or
        $audit -notmatch 'ExceptionTableRVA: 0x0\b' -or $audit -notmatch 'ExceptionTableSize: 0x0\b' -or
        $audit -notmatch 'CLRRuntimeHeaderRVA: 0x0\b' -or $audit -notmatch 'CLRRuntimeHeaderSize: 0x0\b') {
        throw 'OCR ARM64 no-CRT PE contract failed'
    }
    Write-Output "Built $binary; ARM64, Kernel32 only, no CRT, no exception or CLR tables"
    & $binary --self-test
    if ($LASTEXITCODE -ne 0) { throw 'OCR SHA-256 known-answer tests failed' }
    if ($Test) { Test-Ocr }
    if ($TestTokenizer) {
        & $binary --test-tokenizer "$TokenizerDir/tokenizer.got" "$TokenizerDir/tokenizer-fixtures.got"
        if ($LASTEXITCODE -ne 0) { throw 'OCR native tokenizer tests failed' }
    }
    if ($TestImages) {
        & $binary --test-images "$ImageDir/image-fixtures.got" "$ImageDir/position-fixtures.got"
        if ($LASTEXITCODE -ne 0) { throw 'OCR native image tests failed' }
    }
    if ($Download -or $Verify) { Stage-Model }
} finally {
    Pop-Location
}