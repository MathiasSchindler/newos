param(
    [string]$Compiler = 'clang',
    [string]$BuildDir = 'experimental/snapdragon/build/ocr',
    [string]$ModelDir = 'experimental/snapdragon/models/glm-ocr',
    [string]$TokenizerDir = 'experimental/snapdragon/models/glm-ocr-tokenizer-v2',
    [string]$ImageDir = 'experimental/snapdragon/models/glm-ocr-images-v2',
    [string]$QnnDir = 'experimental/snapdragon/build',
    [string]$HtpDir = 'experimental/snapdragon/models/glm-ocr-htp-v1',
    [switch]$Download,
    [switch]$Verify,
    [switch]$Test,
    [switch]$TestTokenizer,
    [switch]$TestImages,
    [switch]$PrepareImages,
    [switch]$Vision,
    [switch]$TestVision,
    [switch]$Prefill,
    [switch]$TestPrefill,
    [switch]$Generate,
    [switch]$TestGenerate,
    [switch]$ReuseDecode,
    [switch]$LargeImages,
    [ValidateRange(1,256)][int]$MaxNewTokens = 3,
    [string]$GenerationDir = 'experimental/snapdragon/models/glm-ocr-generation-v2',
    [string]$TextDir = 'experimental/snapdragon/models/glm-ocr-text-v1',
    [string]$VisionDir = 'experimental/snapdragon/models/glm-ocr-vision-v1',
    [switch]$TestHtp,
    [switch]$CaptureHtp,
    [switch]$CaptureInternals,
    [switch]$RefineDivide,
    [switch]$RefineSiluOnly,
    [switch]$MatrixResidual,
    [switch]$FuseDownResidual,
    [switch]$FuseDownResidualBlock0,
    [switch]$FuseAttentionResidual,
    [switch]$MatrixRope,
    [switch]$SplitRope,
    [switch]$RopeBlock1Only
)

$ErrorActionPreference = 'Stop'
if ($TestGenerate) { $Generate = $true }
if ($Generate) { $Prefill = $true }
if ($LargeImages -and -not $PSBoundParameters.ContainsKey('VisionDir')) { $VisionDir = 'experimental/snapdragon/models/glm-ocr-vision-v2' }
if ($ReuseDecode -and -not $Generate) { throw '-ReuseDecode requires -Generate or -TestGenerate' }
if ($TestVision) { $Vision = $true }
if ($TestPrefill) { $Prefill = $true }
if ($Prefill) { $Vision = $true }
if ($LargeImages -and -not $Vision) { throw '-LargeImages requires -Vision, -Prefill or -Generate' }
if ($Vision -and ($TestHtp -or $CaptureInternals -or $RefineDivide -or $MatrixResidual -or $FuseDownResidual -or $MatrixRope -or $SplitRope -or $RopeBlock1Only)) { throw 'Vision runtime does not accept diagnostic HTP variants' }
if ($FuseDownResidualBlock0 -and -not $FuseDownResidual) { throw '-FuseDownResidualBlock0 requires -FuseDownResidual' }
if ($FuseAttentionResidual -and -not $FuseDownResidual) { throw '-FuseAttentionResidual requires -FuseDownResidual' }
if ($FuseDownResidual -and -not $CaptureInternals) { throw '-FuseDownResidual requires -CaptureInternals' }
if ($MatrixResidual -and -not ($RefineDivide -and $RefineSiluOnly -and $CaptureInternals)) { throw '-MatrixResidual requires -RefineDivide -RefineSiluOnly -CaptureInternals' }
if ($RefineSiluOnly -and -not $RefineDivide) { throw '-RefineSiluOnly requires -RefineDivide' }
if ($CaptureInternals -and -not ($TestHtp -and $CaptureHtp)) { throw '-CaptureInternals requires -TestHtp -CaptureHtp' }
if ($RopeBlock1Only -and -not ($MatrixRope -or $SplitRope)) { throw '-RopeBlock1Only requires -MatrixRope or -SplitRope' }
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

function Assert-Native([string[]]$Arguments, [int]$Expected = 0, [string]$ExpectedMessage = '') {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $messages = & $binary @Arguments 2>&1
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previous }
    if ($code -ne $Expected) { throw ('Native OCR exit {0}, expected {1}: {2}' -f $code,$Expected,($messages -join ' ')) }
    if ($ExpectedMessage -and ($messages -join "`n").IndexOf($ExpectedMessage, [StringComparison]::Ordinal) -lt 0) {
        throw ('Native OCR missing expected diagnostic: {0}' -f $ExpectedMessage)
    }
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

function Test-Generation {
    Assert-Native @('--test-generation',$GenerationDir) 0 'PASS generation weights/tokenizer'
    $scratch = Join-Path $repoRoot ('tests/tmp/ocr-generation-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $scratch | Out-Null
    try {
        Assert-Native @('--test-generation',$scratch) 1
        $path = Join-Path $scratch 'head-00.got'
        $bytes = [IO.File]::ReadAllBytes((Join-Path $GenerationDir 'head-00.got'))
        foreach ($offset in @(0,12,32,96,128,160,($bytes.Length-1))) {
            $bytes[$offset] = $bytes[$offset] -bxor 1
            [IO.File]::WriteAllBytes($path,$bytes)
            Assert-Native @('--test-generation',$scratch) 1
            $bytes[$offset] = $bytes[$offset] -bxor 1
        }
        [IO.File]::WriteAllBytes($path,(New-Object byte[] 127))
        Assert-Native @('--test-generation',$scratch) 1
        [IO.File]::WriteAllBytes($path,$bytes)
        Assert-Native @('--test-generation',$scratch) 1
        foreach ($limit in @('0','257','-1','1x','999999999999999999')) {
            Assert-Native @('--generate-text','missing','missing','missing','missing','missing','missing',$limit) 2
        }
        foreach ($limit in @('64','100','256')) {
            Assert-Native @('--generate-text','missing','missing','missing','missing','missing','missing',$limit) 1 'FAIL vision image input'
        }
        foreach ($artifact in Get-ChildItem -LiteralPath $GenerationDir -Filter '*.got') {
            if ($artifact.Name -ne 'head-00.got') { Copy-Item -LiteralPath $artifact.FullName -Destination (Join-Path $scratch $artifact.Name) }
        }
        $ropePath = Join-Path $scratch 'decode-rope.got'
        $rope = [IO.File]::ReadAllBytes($ropePath)
        foreach ($offset in @(12,96,128,160,($rope.Length-1))) {
            $rope[$offset] = $rope[$offset] -bxor 1
            [IO.File]::WriteAllBytes($ropePath,$rope)
            Assert-Native @('--test-generation',$scratch) 1
            $rope[$offset] = $rope[$offset] -bxor 1
        }
        [IO.File]::WriteAllBytes($ropePath,(New-Object byte[] 127))
        Assert-Native @('--test-generation',$scratch) 1
        Remove-Item -LiteralPath $ropePath
        Assert-Native @('--test-generation',$scratch) 1
        Write-Output 'PASS generation checks: 22 negative head/RoPE/artifact/limit cases and 3 valid extended CLI limits'
    } finally { Remove-Item -LiteralPath $scratch -Recurse -Force }
}

function Test-ImageFiles {
    $scratch = Join-Path $repoRoot ('tests/tmp/ocr-image-files-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $scratch | Out-Null
    $stream = [IO.File]::OpenRead((Join-Path $ImageDir 'image-fixtures.got'))
    $reader = New-Object IO.BinaryReader($stream)
    $hash = [Security.Cryptography.SHA256]::Create()
    $checked = 0
    try {
        $null = $stream.Seek(128, [IO.SeekOrigin]::Begin)
        $geometryCount = $reader.ReadUInt32()
        $imageCount = $reader.ReadUInt32()
        $null = $stream.Seek(16L * $geometryCount, [IO.SeekOrigin]::Current)
        for ($index = 0; $index -lt $imageCount; ++$index) {
            $height = $reader.ReadUInt32(); $width = $reader.ReadUInt32()
            $targetHeight = $reader.ReadUInt32(); $targetWidth = $reader.ReadUInt32()
            $sourceSize = $reader.ReadUInt32(); $targetSize = $reader.ReadUInt32(); $patchSize = $reader.ReadUInt32()
            if ($sourceSize -gt 65536 -or $checked -ge 16) {
                $null = $stream.Seek([long]$sourceSize + $targetSize + $patchSize, [IO.SeekOrigin]::Current)
                continue
            }
            $rgb = $reader.ReadBytes($sourceSize)
            $null = $stream.Seek($targetSize, [IO.SeekOrigin]::Current)
            $patches = $reader.ReadBytes($patchSize)
            if ($rgb.Length -ne $sourceSize -or $patches.Length -ne $patchSize) { throw 'Truncated image test fixture' }
            $expected = [BitConverter]::ToString($hash.ComputeHash($patches)).Replace('-', '')
            [int]$stride = ([int]$width * 3 + 3) -band -4
            foreach ($topDown in @($false, $true)) {
                $inputPath = Join-Path $scratch ('image ' + [char]0x00e4 + '.bmp')
                $outputPath = Join-Path $scratch ('patches-{0}.f32' -f $checked)
                $bitmap = New-Object byte[] (54 + $stride * $height)
                $memory = New-Object IO.MemoryStream(,$bitmap)
                $writer = New-Object IO.BinaryWriter($memory)
                try {
                    $writer.Write([byte]0x42); $writer.Write([byte]0x4d)
                    $writer.Write([uint32]$bitmap.Length); $writer.Write([uint32]0)
                    $writer.Write([uint32]54); $writer.Write([uint32]40); $writer.Write([int]$width)
                    $signedHeight = if ($topDown) { -[int]$height } else { [int]$height }
                    $writer.Write([int]$signedHeight); $writer.Write([uint16]1); $writer.Write([uint16]24)
                    $writer.Write([uint32]0); $writer.Write([uint32]($stride * $height))
                    $writer.Flush()
                } finally { $writer.Dispose(); $memory.Dispose() }
                for ($row = 0; $row -lt $height; ++$row) {
                    $diskRow = if ($topDown) { $row } else { $height - 1 - $row }
                    for ($column = 0; $column -lt $width; ++$column) {
                        $source = ($row * $width + $column) * 3
                        $target = 54 + $diskRow * $stride + $column * 3
                        $bitmap[$target] = $rgb[$source + 2]
                        $bitmap[$target + 1] = $rgb[$source + 1]
                        $bitmap[$target + 2] = $rgb[$source]
                    }
                }
                [IO.File]::WriteAllBytes($inputPath, $bitmap)
                $metadata = & $binary --prepare-image $inputPath $outputPath
                if ($LASTEXITCODE -ne 0) { throw 'BMP file preparation failed' }
                $record = ConvertFrom-Json -InputObject ($metadata -join "`n")
                if ($record.source_height -ne $height -or $record.source_width -ne $width -or
                    $record.height -ne $targetHeight -or $record.width -ne $targetWidth -or
                    $record.grid_height -ne $targetHeight / 14 -or $record.grid_width -ne $targetWidth / 14 -or
                    $record.image_tokens -ne $targetHeight * $targetWidth / 784 -or
                    $record.patches -ne $targetHeight * $targetWidth / 196 -or
                    $record.features -ne 1176 -or $record.float32_values -ne $patchSize / 4 -or
                    (Get-Item -LiteralPath $outputPath).Length -ne $patchSize -or
                    (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash -ne $expected) {
                    throw ('BMP-to-patches differs from oracle fixture {0}' -f $index)
                }
                Assert-Native @('--prepare-image', $inputPath, $outputPath) 1 'FAIL image preparation'
                if ((Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash -ne $expected) { throw 'Output overwritten' }
                ++$checked
            }
        }
        if ($checked -lt 4) { throw 'Insufficient BMP fixture coverage' }
        $absent = Join-Path $scratch 'absent.bmp'
        $unused = Join-Path $scratch 'unused.f32'
        Assert-Native @('--prepare-image', $absent, $unused) 1 'FAIL image preparation'
        Assert-Native @('--prepare-image', $inputPath, (Join-Path $scratch 'absent/output.f32')) 1 'FAIL image preparation'
        Assert-Native @('--prepare-image', $inputPath) 2
        $bitmap[0] = 0
        [IO.File]::WriteAllBytes($inputPath, $bitmap)
        Assert-Native @('--prepare-image', $inputPath, $unused) 1 'FAIL image preparation'
        [IO.File]::WriteAllBytes($inputPath, (New-Object byte[] 53))
        Assert-Native @('--prepare-image', $inputPath, $unused) 1 'FAIL image preparation'
        if (Test-Path -LiteralPath $unused) { throw 'Rejected input created output' }
        Write-Output ('PASS BMP file-to-patch oracle comparisons: {0}; Unicode, geometry, exact float32 bytes, no overwrite, invalid paths/input' -f $checked)
    } finally {
        $reader.Dispose(); $stream.Dispose(); $hash.Dispose()
        Remove-Item -LiteralPath $scratch -Recurse -Force
    }
}

function Test-PrefillInput {
    Assert-Native @('--test-text-input', $TextDir)
    $scratch = Join-Path $repoRoot ('tests/tmp/ocr-prefill-input-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $scratch | Out-Null
    try {
        Assert-Native @('--test-text-input', $scratch) 1
        $path = Join-Path $scratch 'text-00.got'
        $original = [IO.File]::ReadAllBytes((Join-Path $TextDir 'text-00.got'))
        foreach ($offset in @(0,12,96,128,160,($original.Length - 1))) {
            $original[$offset] = $original[$offset] -bxor 1
            [IO.File]::WriteAllBytes($path, $original)
            Assert-Native @('--test-text-input', $scratch) 1
            $original[$offset] = $original[$offset] -bxor 1
        }
        [IO.File]::WriteAllBytes($path, (New-Object byte[] 127))
        Assert-Native @('--test-text-input', $scratch) 1
        [IO.File]::WriteAllBytes($path, $original)
        Assert-Native @('--test-text-input', $scratch) 1
        Assert-Native @('--prefill-text') 2
        Assert-Native @('--prefill-invalid') 2
        Write-Output 'PASS prefill input: 12 exact official-template/embedding/position/mask cases; 11 negative artifact/argument checks'
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
    if ($TestImages -or $PrepareImages -or $Vision) {
        $binary = Join-Path $BuildDir 'ocr-image.exe'
        $flags += @('-DOCR_IMAGE_FILE', '-fno-math-errno', '-ffp-contract=off')
        $sources += @('experimental/snapdragon/src/tools/ocr/ocr_image.c', 'experimental/snapdragon/src/tools/ocr/ocr_image_file.c')
        $sources += @('experimental/snapdragon/src/tools/ocr/ocr_png.c', 'src/shared/compression/zlib.c')
        if ($TestImages -or $Prefill) { $sources += 'experimental/snapdragon/src/tools/ocr/ocr_text.c' }
        if ($TestImages) {
            $binary = Join-Path $BuildDir 'ocr-image-test.exe'
            $flags += '-DOCR_IMAGE_TEST'
        }
    }
    if ($TestHtp -or $Vision) {
        $binary = Join-Path $BuildDir 'ocr-htp-test.exe'
        if ($Vision) { $binary = Join-Path $BuildDir 'ocr-vision.exe'; $flags += '-DOCR_VISION_RUN' }
        if ($Prefill) { $binary = Join-Path $BuildDir 'ocr-prefill.exe'; $flags += '-DOCR_TEXT_DECODER' }
        if ($Generate) { $binary = Join-Path $BuildDir 'ocr-generate.exe' }
        if ($ReuseDecode) { $flags += '-DOCR_REUSE_DECODE' }
        if ($LargeImages) { $flags += '-DOCR_LARGE_IMAGES' }
        if ($MatrixRope -or $SplitRope) { $flags += '-DOCR_MATRIX_ROPE' }
        if ($SplitRope) { $flags += '-DOCR_SPLIT_ROPE' }
        if ($CaptureInternals) { $flags += '-DOCR_CAPTURE_INTERNALS' }
        if ($RefineDivide) { $flags += '-DOCR_REFINE_DIVIDE' }
        if ($RefineSiluOnly) { $flags += '-DOCR_REFINE_SILU_ONLY' }
        if ($MatrixResidual) { $flags += '-DOCR_MATRIX_RESIDUAL' }
        if ($FuseDownResidual) { $flags += '-DOCR_FUSE_DOWN_RESIDUAL' }
        if ($FuseDownResidualBlock0) { $flags += '-DOCR_FUSE_DOWN_RESIDUAL_BLOCK0' }
        if ($FuseAttentionResidual) { $flags += '-DOCR_FUSE_ATTENTION_RESIDUAL' }
        if ($RopeBlock1Only) { $flags += '-DOCR_ROPE_BLOCK1_ONLY' }
        $flags += @('-DOCR_HTP_TEST', '-ffp-contract=off', '-fno-math-errno')
        $sources += 'experimental/snapdragon/src/tools/ocr/ocr_htp.c'
        if (-not ($TestTokenizer -or $TestImages)) { $sources += 'experimental/snapdragon/src/tools/ocr/ocr_tokenizer.c' }
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
        Test-ImageFiles
    }
    if ($PrepareImages -and $Test -and -not $TestImages) { Test-ImageFiles }
    if ($Prefill -and ($Test -or $TestPrefill -or $TestGenerate)) { Test-PrefillInput }
    if ($Generate -and ($Test -or $TestGenerate)) { Test-Generation }
    if ($TestVision -or $TestPrefill -or $TestGenerate) {
        Assert-Native @('--check-vision', $VisionDir)
        $negative = Join-Path $repoRoot ('tests/tmp/ocr-vision-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $negative | Out-Null
        try {
            Assert-Native @('--check-vision', $negative) 1 'FAIL vision weight artifact'
            $firstBlock = Join-Path $negative 'block-00.got'
            $original = [IO.File]::ReadAllBytes((Join-Path $VisionDir 'block-00.got'))
            foreach ($offset in @(0,12,32,96,128,160,($original.Length - 1))) {
                $original[$offset] = $original[$offset] -bxor 1
                [IO.File]::WriteAllBytes($firstBlock, $original)
                Assert-Native @('--check-vision', $negative) 1 'FAIL vision weight artifact'
                $original[$offset] = $original[$offset] -bxor 1
            }
            [IO.File]::WriteAllBytes($firstBlock, (New-Object byte[] 127))
            Assert-Native @('--check-vision', $negative) 1 'FAIL vision weight artifact'
            [IO.File]::WriteAllBytes($firstBlock, $original)
            Assert-Native @('--check-vision', $negative) 1 'FAIL vision weight artifact'
            $missingDll = Join-Path $negative 'missing.dll'
            $missingImage = Join-Path $negative 'missing.bmp'
            Assert-Native @('--vision', $missingDll, $VisionDir, $missingImage, $negative) 1 'FAIL vision image input'
            $bitmap = New-Object byte[] (54 + 56 * 112 * 3)
            $bitmap[0] = 0x42; $bitmap[1] = 0x4d; $bitmap[26] = 1; $bitmap[28] = 24
            foreach ($field in @(@(2,$bitmap.Length),@(10,54),@(14,40),@(18,56),@(22,112))) {
                [Array]::Copy([BitConverter]::GetBytes([uint32]$field[1]),0,$bitmap,$field[0],4)
            }
            [IO.File]::WriteAllBytes($missingImage, $bitmap)
            Assert-Native @('--vision', $missingDll, $VisionDir, $missingImage, $negative) 1 'FAIL vision bucket or weights'
            Assert-Native @('--vision', $missingDll, $VisionDir, (Join-Path $VisionDir 'pattern.bmp'), $negative) 1 'FAIL loading explicit HTP library'
            Assert-Native @('--vision') 2
            Write-Output 'PASS vision negative checks: 14; missing/corrupt/truncated weights, invalid image/bucket/runtime/arguments'
            if ($LargeImages) {
                foreach ($artifact in Get-ChildItem -LiteralPath $VisionDir -Filter '*.got') {
                    if ($artifact.Name -notin @('block-00.got','large-rope.got')) {
                        New-Item -ItemType HardLink -Path (Join-Path $negative $artifact.Name) -Target $artifact.FullName | Out-Null
                    }
                }
                Assert-Native @('--check-vision', $negative) 1
                $ropePath = Join-Path $negative 'large-rope.got'
                [byte[]]$rope = [IO.File]::ReadAllBytes((Join-Path $VisionDir 'large-rope.got'))
                [IO.File]::WriteAllBytes($ropePath, (New-Object byte[] 127))
                Assert-Native @('--check-vision', $negative) 1
                $rope[160] = $rope[160] -bxor 1
                [IO.File]::WriteAllBytes($ropePath, $rope)
                Assert-Native @('--check-vision', $negative) 1
                [Array]::Copy([BitConverter]::GetBytes([single]::NaN),0,$rope,160,4)
                $hash = [Security.Cryptography.SHA256]::Create()
                try { [Array]::Copy($hash.ComputeHash([byte[]]($rope[0..95]+$rope[128..($rope.Length-1)])),0,$rope,96,32) }
                finally { $hash.Dispose() }
                [IO.File]::WriteAllBytes($ropePath, $rope)
                Assert-Native @('--check-vision', $negative) 1
                Copy-Item -LiteralPath (Join-Path $VisionDir 'large-rope.got') -Destination $ropePath -Force
                Assert-Native @('--check-vision', $negative)
                Write-Output 'PASS large-grid RoPE: missing/truncated/corrupt/rehashed nonfinite rejected; restored artifact accepted'
            }
        } finally { Remove-Item -LiteralPath $negative -Recurse -Force }
        $weightHashes = [ordered]@{}
        foreach ($artifact in Get-ChildItem -LiteralPath $VisionDir -Filter '*.got') { $weightHashes[$artifact.Name] = (Get-FileHash -LiteralPath $artifact.FullName).Hash }
        $runtimeHashes = [ordered]@{}
        foreach ($name in @('QnnHtp.dll','QnnHtpPrepare.dll','QnnHtpV73Stub.dll','libQnnHtpV73Skel.so')) {
            $runtimeHashes[$name] = (Get-FileHash -LiteralPath (Join-Path $QnnDir $name)).Hash
        }
        foreach ($case in @('pattern', 'receipt')) {
            $inputImage = Join-Path $VisionDir ($case + $(if ($LargeImages) { '.png' } else { '.bmp' }))
            $capture = Join-Path $BuildDir ($case + '-' + [guid]::NewGuid().ToString('N'))
            New-Item -ItemType Directory -Path $capture | Out-Null
            $runtime = (Resolve-Path (Join-Path $QnnDir 'QnnHtp.dll')).Path
            $previous = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            try {
                if ($TestGenerate) {
                    $runArgs = @('--generate-text',$runtime,$VisionDir,$TextDir,$GenerationDir,$inputImage,$capture,[string]$MaxNewTokens)
                    $quoted = @($runArgs | ForEach-Object { '"' + $_ + '"' })
                    $process = Start-Process -FilePath $binary -ArgumentList $quoted -NoNewWindow -Wait -PassThru -RedirectStandardOutput (Join-Path $capture 'stdout.txt') -RedirectStandardError (Join-Path $capture 'execution.log')
                    $code = $process.ExitCode
                    Write-Output ('Generation {0}: exit {1}, capture {2}' -f $case,$code,$capture)
                    Get-Content -LiteralPath (Join-Path $capture 'execution.log') -Tail 8
                } else {
                    $runArgs = if ($TestPrefill) { @('--prefill-text',$runtime,$VisionDir,$TextDir,$inputImage,$capture) } else { @('--vision',$runtime,$VisionDir,$inputImage,$capture) }
                    & $binary @runArgs 2>&1 | Tee-Object -FilePath (Join-Path $capture 'execution.log')
                    $code = $LASTEXITCODE
                }
            } finally { $ErrorActionPreference = $previous }
            $captureHashes = [ordered]@{}
            foreach ($tensor in Get-ChildItem -LiteralPath $capture -File | Where-Object { $_.Extension -in @('.f16','.f32','.u32','.i32','.u8','.u64','.txt') }) {
                $captureHashes[$tensor.Name] = (Get-FileHash -LiteralPath $tensor.FullName).Hash
            }
            $textHashes = [ordered]@{}
            if ($TestPrefill -or $TestGenerate) { foreach ($artifact in Get-ChildItem -LiteralPath $TextDir -Filter '*.got') { $textHashes[$artifact.Name] = (Get-FileHash -LiteralPath $artifact.FullName).Hash } }
            $generationHashes = [ordered]@{}
            if ($TestGenerate) { foreach ($artifact in Get-ChildItem -LiteralPath $GenerationDir -Filter '*.got') { $generationHashes[$artifact.Name] = (Get-FileHash -LiteralPath $artifact.FullName).Hash } }
            $runType = if ($TestGenerate) { 'generate' } elseif ($TestPrefill) { 'prefill' } else { 'vision' }
            [ordered]@{ case = $case; exit_code = $code; capture = (Resolve-Path $capture).Path; text_weight_hashes = $textHashes;
                generation_weight_hashes = $generationHashes; max_new_tokens = $MaxNewTokens; reuse_decode = [bool]$ReuseDecode;
                large_images = [bool]$LargeImages; prefill_context = $(if ($LargeImages) { 256 } else { 64 });
                executable_sha256 = (Get-FileHash $binary).Hash; runtime_sha256 = (Get-FileHash $runtime).Hash;
                runtime_hashes = $runtimeHashes; weight_hashes = $weightHashes; captures = $captureHashes;
                input_sha256 = (Get-FileHash -LiteralPath $inputImage).Hash;
                scope = $runType; numerical_acceptance = $false } |
                ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 (Join-Path $BuildDir ($case + '-' + $runType + '-run.json'))
            if ($code -ne 0 -and -not ($TestGenerate -and $code -eq 3)) { throw ('Native execution failed: {0}; capture retained' -f $case) }
        }
    }
    if ($Download -or $Verify) { Stage-Model }
    if ($TestHtp) {
        $runtime = (Resolve-Path $QnnDir).Path
        $binaryPath = (Resolve-Path $binary).Path
        $fixturePath = (Resolve-Path "$HtpDir/htp-fixtures.got").Path
        $negativeDir = Join-Path $repoRoot ('tests/tmp/ocr-htp-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $negativeDir | Out-Null
        try {
            $missing = Join-Path $negativeDir 'missing.dll'
            Assert-Native @('--test-htp', $missing, (Join-Path $negativeDir 'missing.got')) 1 'FAIL HTP fixture verification'
            Assert-Native @('--test-htp', $missing, $fixturePath) 1 'FAIL loading explicit HTP library'
            $damaged = [IO.File]::ReadAllBytes($fixturePath)
            $damaged[$damaged.Length - 1] = $damaged[$damaged.Length - 1] -bxor 1
            $badFixture = Join-Path $negativeDir 'corrupt.got'
            [IO.File]::WriteAllBytes($badFixture, $damaged)
            Assert-Native @('--test-htp', $missing, $badFixture) 1 'FAIL HTP fixture verification'
            [IO.File]::WriteAllBytes($badFixture, (New-Object byte[] 127))
            Assert-Native @('--test-htp', $missing, $badFixture) 1 'FAIL HTP fixture verification'
            $negativeCount = 4
            $originalFixture = [IO.File]::ReadAllBytes($fixturePath)
            $fixtureKind = [BitConverter]::ToUInt32($originalFixture,12)
            if ($fixtureKind -in @(6,7,8,9)) {
                $invalidOffsets = if ($fixtureKind -in @(8,9)) { @(128,164,168,172,176,180,184,188) } else { @(128,168) }
                if ($fixtureKind -eq 9) {
                    $secondRecord = 192+[BitConverter]::ToUInt32($originalFixture,180)+[BitConverter]::ToUInt32($originalFixture,184)+[BitConverter]::ToUInt32($originalFixture,188)
                    $invalidOffsets += @(160,$secondRecord,($secondRecord+4),($secondRecord+8),($secondRecord+12),($secondRecord+16),($secondRecord+20),($secondRecord+24))
                }
                foreach ($offset in $invalidOffsets) {
                    $damaged = [byte[]]$originalFixture.Clone()
                    $damaged[$offset] = $damaged[$offset] -bxor 1
                    $hash = [Security.Cryptography.SHA256]::Create()
                    try {
                        $null = $hash.TransformBlock($damaged,0,96,$damaged,0)
                        $null = $hash.TransformBlock($damaged,128,$damaged.Length-128,$damaged,128)
                        $null = $hash.TransformFinalBlock((New-Object byte[] 0),0,0)
                        [Array]::Copy($hash.Hash,0,$damaged,96,32)
                    } finally { $hash.Dispose() }
                    [IO.File]::WriteAllBytes($badFixture,$damaged)
                    $diagnostic = if ($offset -eq 128) { 'FAIL learned weight identity' } else { 'FAIL HTP fixture structure' }
                    Assert-Native @('--test-htp',$missing,$badFixture) 1 $diagnostic
                    ++$negativeCount
                }
            }
            Write-Output ('PASS HTP negative checks: {0}' -f $negativeCount)
        } finally { Remove-Item -LiteralPath $negativeDir -Recurse -Force }
        $logPath = Join-Path (Resolve-Path $BuildDir).Path 'htp-probe.log'
        $reportPath = Join-Path (Resolve-Path $BuildDir).Path 'htp-probe.json'
        $runtimeFiles = @('QnnHtp.dll','QnnHtpPrepare.dll','QnnHtpV73Stub.dll','libQnnHtpV73Skel.so')
        $identities = @{}
        foreach ($name in $runtimeFiles) {
            $identities[$name] = (Get-FileHash -LiteralPath (Join-Path $runtime $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        $oldPath = $env:PATH
        $captureDirectory = $null
        $nativeArguments = @('--test-htp',(Join-Path $runtime 'QnnHtp.dll'),$fixturePath)
        if ($CaptureHtp) {
            $captureDirectory = Join-Path (Resolve-Path $BuildDir).Path ('capture-' + [guid]::NewGuid().ToString('N'))
            New-Item -ItemType Directory -Path $captureDirectory | Out-Null
            $nativeArguments = @('--capture-htp',(Join-Path $runtime 'QnnHtp.dll'),$fixturePath,$captureDirectory)
        }
        $oldPreference = $ErrorActionPreference
        Push-Location $runtime
        try {
            $env:PATH = $runtime + ';' + $oldPath
            $ErrorActionPreference = 'Continue'
            & $binaryPath @nativeArguments 2>&1 | ForEach-Object { $_.ToString() } | Tee-Object -FilePath $logPath
            $probeExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $oldPreference
            $env:PATH = $oldPath
            Pop-Location
        }
        $report = [ordered]@{
            schema_version = 1; exit_code = $probeExit; runtime_directory = $runtime; runtime_sha256 = $identities
            executable_sha256 = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
            fixtures_sha256 = (Get-FileHash -LiteralPath $fixturePath -Algorithm SHA256).Hash.ToLowerInvariant()
            full_model_inference = $false; timestamp_utc = [DateTime]::UtcNow.ToString('o')
            rope_implementation = $(if ($SplitRope) { 'split-matrix' } elseif ($MatrixRope) { 'matrix' } else { 'elementwise' })
            rope_block1_only = [bool]$RopeBlock1Only
            capture_internals = [bool]$CaptureInternals
            refine_divide_block1 = [bool]$RefineDivide
            refine_silu_only = [bool]$RefineSiluOnly
            matrix_residual = [bool]$MatrixResidual
            fuse_down_residual_block1 = [bool]$FuseDownResidual
            fuse_down_residual_block0 = [bool]$FuseDownResidualBlock0
            fuse_attention_residual_block1 = [bool]$FuseAttentionResidual
            matrix_residual_group = $(if ($MatrixResidual) { 256 } else { 0 })
        }
        if ($CaptureHtp) {
            $captures = @{}
            Get-ChildItem -LiteralPath $captureDirectory -Filter '*.f16' | ForEach-Object { $captures[$_.Name] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
            $report.capture_directory = $captureDirectory
            $report.capture_sha256 = $captures
        }
        $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $reportPath -Encoding UTF8
        if ($probeExit -ne 0) { throw 'OCR HTP probe failed; see htp-probe.log' }
    }
} finally {
    Pop-Location
}