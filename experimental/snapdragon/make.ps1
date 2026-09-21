[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidateSet('build','clean','rebuild','status','test','distro','test-distro')][string]$Target = 'build',
    [string]$Compiler = 'clang',
    [string]$DistroDir = '',
    [ValidateSet('all','translate')][string]$DistroApp = 'all',
    [string]$TranslateBuildDir = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
if (-not $DistroDir) { $DistroDir = Join-Path $projectRoot 'distro' }
$buildRoot = Join-Path $projectRoot 'build'
if ($TranslateBuildDir -and $DistroApp -ne 'translate') { throw '-TranslateBuildDir requires -DistroApp translate' }
if (-not $TranslateBuildDir) { $TranslateBuildDir = $buildRoot }
$TranslateBuildDir = [IO.Path]::GetFullPath($TranslateBuildDir)
$script:distroTranslateBits = 4
$stateRoot = Join-Path $projectRoot 'data/build-state'
$repoRoot = (Resolve-Path (Join-Path $projectRoot '../..')).Path

function Get-BuildFiles([string]$Directory) {
    if (-not (Test-Path -LiteralPath $Directory)) { return }
    $pending = New-Object 'Collections.Generic.Stack[string]'
    $pending.Push($Directory)
    while ($pending.Count) {
        $current = [IO.DirectoryInfo]::new($pending.Pop())
        if ($current.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw ('Refusing reparse point: ' + $current.FullName) }
        foreach ($item in $current.EnumerateFileSystemInfos()) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw ('Refusing reparse point: ' + $item.FullName) }
            if ($item.Attributes -band [IO.FileAttributes]::Directory) { $pending.Push($item.FullName) } else { $item }
        }
    }
}

function Get-RelativeBuildPath([string]$Path) {
    return $Path.Substring($buildRoot.Length + 1).Replace('\','/')
}

function Test-DevelopmentPath([string]$Path) {
    return $Path -match '^(calibration-venv|gemma-oracle-x64|ocr-oracle)/'
}

function Test-PreservePath([string]$Path) {
    if ([IO.Path]::GetExtension($Path).ToLowerInvariant() -in '.pyc','.pyo') { return $false }
    if (Test-DevelopmentPath $Path) { return $true }
    if ($Path -match '^ocr-app/graph-cache/') { return $true }
    return [IO.Path]::GetExtension($Path).ToLowerInvariant() -notin '.exe','.dll','.so','.cat','.obj','.o','.a','.lib','.pdb','.ilk','.qob','.qoc'
}

function Test-RestorePath([string]$Path) {
    return (Test-DevelopmentPath $Path) -or $Path -match '^gemma-block/prompt-512\.gmb(?:\.[a-z.]+|\.part-[0-2]\.bundle\.context)?$|^[^/]+\.qnnctx$|^ocr-app/(graph-cache/|runs/|[^/]+\.(png|bmp)$)'
}

function Assert-PlainAncestors([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw ('Refusing reparse point: ' + $current) }
        }
        $current = Split-Path -Parent $current
    }
}

function Get-ActiveBuildProcesses {
    $prefix = $buildRoot.TrimEnd('\') + '\'
    Get-CimInstance Win32_Process | Where-Object {
        $_.ProcessId -ne $PID -and (
            ($_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) -or
            ($_.CommandLine -and ($_.CommandLine.IndexOf($prefix, [StringComparison]::OrdinalIgnoreCase) -ge 0)))
    }
}

function Assert-BuildIdle {
    $active = @(Get-ActiveBuildProcesses)
    if ($active.Count) { throw ('Close build applications/processes first: ' + (($active | ForEach-Object { '{0} (PID {1})' -f $_.Name,$_.ProcessId }) -join ', ')) }
}

function Get-Digest([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hash = [Security.Cryptography.SHA256Cng]::new()
    try { return ([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-', '') }
    finally { $hash.Dispose(); $stream.Dispose() }
}

function Read-State {
    $manifest = Join-Path $stateRoot 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifest)) { return @{} }
    $state = ConvertFrom-Json ([IO.File]::ReadAllText($manifest))
    if ($state.version -ne 1) { throw 'Unsupported build-state manifest' }
    $records = @{}
    foreach ($record in $state.files) {
        if ($record.path -notmatch '^[^:\\]+$' -or $record.path.StartsWith('/') -or
            @($record.path.Split('/') | Where-Object {
                $_ -in '', '.', '..' -or $_ -match '[. ]$|[<>"|?*\x00-\x1f]' -or
                $_ -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)'
            }).Count -or
            $record.sha256 -notmatch '^[0-9A-Fa-f]{64}$' -or $records.ContainsKey($record.path)) {
            throw 'Invalid build-state manifest entry'
        }
        $records[$record.path] = $record
    }
    return $records
}

function Save-BuildState([object[]]$Files) {
    Assert-PlainAncestors $stateRoot
    $null = @(Get-BuildFiles $stateRoot)
    $records = Read-State
    $history = [guid]::NewGuid().ToString('N')
    $saved = 0
    $checked = 0
    foreach ($file in $Files) {
        $relative = Get-RelativeBuildPath $file.FullName
        if (-not (Test-PreservePath $relative)) { continue }
        $destination = [IO.Path]::Combine($stateRoot, 'files', $relative)
        $digest = Get-Digest $file.FullName
        $same = [IO.File]::Exists($destination) -and (Get-Digest $destination) -eq $digest
        if (-not $same) {
            $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
            $temporary = [IO.Path]::Combine($stateRoot, 'transfer-' + $history + '.tmp')
            if ([IO.File]::Exists($temporary)) { throw ('Unfinished transfer: ' + $temporary) }
            [IO.File]::Copy($file.FullName, $temporary)
            if ((Get-Digest $temporary) -ne $digest) { throw ('Preservation checksum mismatch: ' + $relative) }
            if ([IO.File]::Exists($destination)) {
                $previous = [IO.Path]::Combine($stateRoot, 'history', ($history + '-' + $saved + '.bin'))
                $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($previous))
                $description = [ordered]@{ path = $relative; sha256 = (Get-Digest $destination) } | ConvertTo-Json
                [IO.File]::WriteAllText(($previous + '.json'), $description, (New-Object Text.UTF8Encoding($false)))
                [IO.File]::Move($destination, $previous)
            }
            [IO.File]::Move($temporary, $destination)
            ++$saved
        }
        $records[$relative] = [ordered]@{ path = $relative; bytes = $file.Length; sha256 = $digest }
        ++$checked
        if (($checked % 10000) -eq 0) { Write-Output ('Preservation verified: {0} files' -f $checked) }
    }
    New-Item -ItemType Directory -Force $stateRoot | Out-Null
    $manifest = Join-Path $stateRoot 'manifest.json'
    $json = [ordered]@{ version = 1; files = @($records.Values | Sort-Object { $_.path }) } | ConvertTo-Json -Depth 4
    $temporaryManifest = Join-Path $stateRoot ('manifest-' + $history + '.tmp')
    [IO.File]::WriteAllText($temporaryManifest, $json, (New-Object Text.UTF8Encoding($false)))
    if (Test-Path -LiteralPath $manifest) { [IO.File]::Replace($temporaryManifest, $manifest, ($manifest + '.previous')) }
    else { Move-Item -LiteralPath $temporaryManifest -Destination $manifest }
    Write-Output ('Preserved {0} files ({1} new/changed), SHA-256 verified: {2}' -f $records.Count,$saved,$stateRoot)
}

function Remove-Build {
    if (-not (Test-Path -LiteralPath $buildRoot)) { Write-Output 'Already clean: build does not exist'; return }
    $before = @(Get-BuildFiles $buildRoot)
    $signatures = @{}
    foreach ($file in $before) { $signatures[$file.FullName] = '{0}/{1}' -f $file.Length,$file.LastWriteTimeUtc.Ticks }
    Save-BuildState $before
    Assert-BuildIdle
    $after = @(Get-BuildFiles $buildRoot)
    if ($after.Count -ne $before.Count) { throw 'Build changed during preservation; refusing deletion' }
    foreach ($file in $after) {
        if ($signatures[$file.FullName] -ne ('{0}/{1}' -f $file.Length,$file.LastWriteTimeUtc.Ticks)) { throw ('Build changed during preservation: ' + $file.FullName) }
    }
    Remove-Item -LiteralPath $buildRoot -Recurse -Force
    if (Test-Path -LiteralPath $buildRoot) { throw 'Build directory was not completely removed' }
    Write-Output 'PASS clean: build removed; models, data and preserved state retained'
}

function Restore-BuildState {
    Assert-PlainAncestors $stateRoot
    $null = @(Get-BuildFiles $stateRoot)
    $records = Read-State
    $restored = 0
    $transfer = [IO.Path]::Combine($buildRoot, 'restore-' + [guid]::NewGuid().ToString('N') + '.tmp')
    foreach ($record in $records.Values) {
        if (-not (Test-RestorePath $record.path)) { continue }
        $destination = [IO.Path]::Combine($buildRoot, $record.path)
        if ([IO.File]::Exists($destination)) { continue }
        $source = [IO.Path]::Combine($stateRoot, 'files', $record.path)
        $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
        try {
            [IO.File]::Copy($source, $transfer)
            if ((Get-Digest $transfer) -ne $record.sha256) { throw ('Restored artifact checksum mismatch: ' + $record.path) }
            [IO.File]::Move($transfer, $destination)
        } finally {
            if ([IO.File]::Exists($transfer)) { [IO.File]::Delete($transfer) }
        }
        ++$restored
        if (($restored % 10000) -eq 0) { Write-Output ('Restore verified: {0} files' -f $restored) }
    }
    Write-Output ('Restored {0} runtime/development files; other evidence stays outside build' -f $restored)
}

function Build-Applications {
    Restore-BuildState
    Push-Location $repoRoot
    try {
        & (Join-Path $projectRoot 'tools/shared/fetch-qnn-runtime.ps1') -BuildDir $buildRoot
        $whisperBuild = Join-Path $buildRoot 'whisper'
        & (Join-Path $projectRoot 'tools/whisper/build.ps1') -Compiler $Compiler -BuildDir $whisperBuild -SelfFusionCandidate
        foreach ($name in 'probe.exe','npu_probe.exe','npu_probe_builder.exe','decoder_kernel_benchmark.exe') {
            Copy-Item -LiteralPath (Join-Path $whisperBuild $name) -Destination (Join-Path $buildRoot $name) -Force
        }
        & (Join-Path $projectRoot 'tools/translate/build-gemma.ps1') -Compiler $Compiler -BuildDir $buildRoot -Translate
        & (Join-Path $projectRoot 'tools/translate/build-gemma.ps1') -Compiler $Compiler -BuildDir $buildRoot -Gui
        $ocrBuild = Join-Path $buildRoot 'ocr-app'
        & (Join-Path $projectRoot 'tools/ocr/build-ocr.ps1') -Compiler $Compiler -BuildDir $ocrBuild -Generate -LargeImages -ReuseDecode -GraphCache -AppMode
        & (Join-Path $projectRoot 'tools/ocr/build-ocr.ps1') -Compiler $Compiler -BuildDir $ocrBuild -Gui
        foreach ($name in 'npu_probe.exe','translate.exe','translate-gui.exe','ocr-app/ocr-generate.exe','ocr-app/ocr-gui.exe') {
            if (-not (Test-Path (Join-Path $buildRoot $name) -PathType Leaf)) { throw ('Missing build output: ' + $name) }
        }
        Write-Output 'PASS build: Whisper, TranslateGemma CLI/GUI and OCR engine/GUI'
    } finally { Pop-Location }
}

function Test-CleanContract {
    $originalBuild = $script:buildRoot
    $originalState = $script:stateRoot
    $scratch = Join-Path $repoRoot ('tests/tmp/snapdragon-clean-' + [guid]::NewGuid().ToString('N'))
    try {
        $script:buildRoot = Join-Path $scratch 'build'
        $script:stateRoot = Join-Path $scratch 'data/build-state'
        $longPython = 'calibration-venv/package/' + ('x' * 105) + '.py'
        foreach ($name in @('translate.exe','old.obj','unknown-model.bin','gemma-block/prompt-512.gmb','gemma-block/prompt-512.gmb.bundle.context','gemma-block/prompt-512.gmb.part-0.bundle.context','gemma-block/prompt-512.gmb.part-1.bundle.context','gemma-block/prompt-512.gmb.part-2.bundle.context','calibration-venv/Scripts/python.exe','calibration-venv/__pycache__/generated.pyc','ocr-app/runs/input.png','ocr-app/graph-cache/2-00.qob','obsolete/2-00.qob','evidence/report.json',$longPython)) {
            $path = Join-Path $buildRoot $name
            New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
            [IO.File]::WriteAllText($path, $name)
        }
        $model = Join-Path $scratch 'models/original.bin'
        New-Item -ItemType Directory -Force (Split-Path -Parent $model) | Out-Null
        [IO.File]::WriteAllText($model, 'abc')
        $modelHash = Get-Digest $model
        if ($modelHash -ne 'BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD') { throw 'SHA-256 known-answer test failed' }
        Remove-Build
        if (Test-Path $buildRoot) { throw 'Clean left build behind' }
        if ((Get-Digest $model) -ne $modelHash) { throw 'Clean changed original model' }
        $state = Read-State
        if ($state.Count -ne 11 -or -not $state.ContainsKey('unknown-model.bin')) { throw 'Preservation classification failed' }
        Restore-BuildState
        if ((Get-BuildFiles $buildRoot | Measure-Object).Count -ne 9 -or (Test-Path (Join-Path $buildRoot 'translate.exe'))) { throw 'Restore included disposable output or omitted input' }
        foreach ($partition in 0..2) {
            $name = 'gemma-block/prompt-512.gmb.part-{0}.bundle.context' -f $partition
            if ([IO.File]::ReadAllText((Join-Path $buildRoot $name)) -ne $name) { throw 'Partition context was not restored intact' }
        }
        $binding = Join-Path $buildRoot 'gemma-block/prompt-512.gmb'
        [IO.File]::WriteAllText($binding, 'updated binding')
        [IO.File]::WriteAllText((Join-Path $buildRoot $longPython), 'updated long source')
        Remove-Build
        if ((Get-BuildFiles (Join-Path $stateRoot 'history') | Measure-Object).Count -ne 4) { throw 'Changed artifact history missing' }
        Restore-BuildState
        if ([IO.File]::ReadAllText($binding) -ne 'updated binding') { throw 'Latest binding not restored' }
        $savedBinding = Join-Path $stateRoot 'files/gemma-block/prompt-512.gmb'
        [IO.File]::WriteAllText($savedBinding, 'corrupt')
        Remove-Item $binding
        $rejected = $false
        try { Restore-BuildState } catch { $rejected = $_.Exception.Message -like '*checksum mismatch*' }
        if (-not $rejected -or (Test-Path $binding)) { throw 'Corrupt saved artifact accepted or published' }
        $manifest = Join-Path $stateRoot 'manifest.json'
        foreach ($invalidPath in '../escape.bin','.. /escape.bin','dir/../escape.bin','dir//escape.bin','NUL','CON.txt','dir/file:stream') {
            $badManifest = @{ version = 1; files = @(@{ path = $invalidPath; sha256 = ('0' * 64) }) } | ConvertTo-Json -Depth 4
            [IO.File]::WriteAllText($manifest, $badManifest)
            $rejected = $false
            try { $null = Read-State } catch { $rejected = $_.Exception.Message -like '*Invalid build-state*' }
            if (-not $rejected) { throw ('Invalid manifest path accepted: ' + $invalidPath) }
        }
        $junction = Join-Path $buildRoot 'outside'
        New-Item -ItemType Junction -Path $junction -Target (Split-Path -Parent $model) | Out-Null
        try {
            $rejected = $false
            try { $null = @(Get-BuildFiles $buildRoot) } catch { $rejected = $_.Exception.Message -like '*reparse point*' }
            if (-not $rejected -or (Get-Digest $model) -ne $modelHash) { throw 'Reparse-point protection failed' }
        } finally { [IO.Directory]::Delete($junction) }
        Write-Output 'PASS clean contract: complete removal, unchanged model, preservation, selective restore, history, corruption/traversal/reparse rejection'
    } finally {
        $script:buildRoot = $originalBuild
        $script:stateRoot = $originalState
        if (Test-Path $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
    }
}

function Read-DistroBindings([string]$Path) {
    $wanted = @('language_model.model.embed_tokens.weight','fixture/prompt/local-cos','fixture/prompt/local-sin','fixture/prompt/global-cos','fixture/prompt/global-sin')
    $found = @{}
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream, [Text.UTF8Encoding]::new($false, $true))
    try {
        if ($stream.Length -lt 20 -or $stream.Length -gt 2097152 -or $reader.ReadUInt32() -ne 0x36424d47 -or
            $reader.ReadUInt32() -ne 2 -or $reader.ReadUInt32() -ne 34) { throw 'Invalid distribution binding header' }
        $bits = $reader.ReadUInt32()
        if ($bits -notin 4,8) { throw 'Invalid distribution binding precision' }
        $count = $reader.ReadUInt32()
        if ($count -gt 1024) { throw 'Invalid distribution binding count' }
        for ($index = 0; $index -lt $count; ++$index) {
            $nameSize = $reader.ReadUInt32(); $pathSize = $reader.ReadUInt32()
            if (-not $nameSize -or $nameSize -ge 192 -or -not $pathSize -or $pathSize -ge 1024 -or
                $nameSize + $pathSize -gt $stream.Length - $stream.Position) { throw 'Invalid distribution binding record' }
            $name = [Text.UTF8Encoding]::new($false, $true).GetString($reader.ReadBytes($nameSize))
            $source = [Text.UTF8Encoding]::new($false, $true).GetString($reader.ReadBytes($pathSize))
            if ($name.Contains([string][char]0) -or $source.Contains([string][char]0)) { throw 'Invalid distribution binding string' }
            if ($name -in $wanted) {
                if ($found.ContainsKey($name)) { throw 'Duplicate distribution binding' }
                if (-not [IO.Path]::IsPathRooted($source)) {
                    $base = if ($source -match '^\.\.?[/\\]') { Split-Path -Parent $Path } else { $repoRoot }
                    $source = Join-Path $base $source
                }
                $found[$name] = [IO.Path]::GetFullPath($source)
            }
        }
        if ($stream.Position -ne $stream.Length -or $found.Count -ne $wanted.Count) { throw 'Missing runtime binding or trailing bytes' }
        foreach ($name in $wanted) { [pscustomobject]@{ name = $name; source = $found[$name]; bits = $bits } }
    } finally { $reader.Dispose() }
}

function New-DistroBindingBytes([object[]]$Bindings, [ValidateSet(4,8)][int]$Bits = 4) {
    $stream = [IO.MemoryStream]::new()
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        foreach ($value in @(0x36424d47,2,34,$Bits,$Bindings.Count)) { $writer.Write([uint32]$value) }
        foreach ($binding in $Bindings) {
            $name = [Text.Encoding]::UTF8.GetBytes($binding.name)
            $path = [Text.Encoding]::UTF8.GetBytes($binding.path)
            $writer.Write([uint32]$name.Length); $writer.Write([uint32]$path.Length)
            $writer.Write($name); $writer.Write($path)
        }
        $writer.Flush()
        return ,$stream.ToArray()
    } finally { $writer.Dispose() }
}

function Get-DistroPlan {
    $plan = [Collections.Generic.List[object]]::new()
    function Add-File([string]$App, [string]$Source, [string]$Path, [string]$Purpose) {
        if ($DistroApp -ne 'all' -and $App -ne $DistroApp) { return }
        $sourcePath = if ([IO.Path]::IsPathRooted($Source)) { [IO.Path]::GetFullPath($Source) } else { [IO.Path]::GetFullPath((Join-Path $projectRoot $Source)) }
        Assert-PlainAncestors $sourcePath
        if (-not [IO.File]::Exists($sourcePath)) { throw ('Missing distribution input: ' + $sourcePath) }
        $plan.Add([pscustomobject]@{ app = $App; source = $sourcePath; path = $Path; purpose = $Purpose; data = $null })
    }
    foreach ($app in 'whisper','translate','ocr') {
        foreach ($name in 'QnnHtp.dll','QnnHtpV73Stub.dll','libQnnHtpV73Skel.so','libqnnhtpv73.cat') {
            Add-File $app ('build/' + $name) ('bin/' + $name) 'Qualcomm V73 inference runtime'
        }
        foreach ($name in 'LICENSE.pdf','NOTICE.txt','NOTICE_WINDOWS.txt') {
            Add-File $app ('build/qnn-licenses/' + $name) ('licenses/qnn/' + $name) 'Qualcomm license/notice (not read by inference)'
        }
        Add-File $app 'build/qnn-runtime-version.txt' 'licenses/qnn/version.txt' 'Pinned runtime provenance (not read by inference)'
        Add-File $app '../../LICENSE' 'licenses/newos.txt' 'Application source license (not read by inference)'
    }
    Add-File 'whisper' 'build/npu_probe.exe' 'bin/whisper.exe' 'Whisper Medium CLI'
    Add-File 'whisper' 'build/whisper-medium-m1c-self-fused-v1-encoder-fp16-l24.qnnctx' 'bin/whisper-medium-m1c-self-fused-v1-encoder-fp16-l24.qnnctx' 'Compiled encoder/decoder NPU weights, default m1c self-fusion mode'
    foreach ($name in 'weights-fp16.bin','token-bytes.bin') {
        Add-File 'whisper' ('models/whisper-medium/decoder-fp16/' + $name) ('models/whisper-medium/decoder-fp16/' + $name) 'CPU decoder weights/token bytes'
    }
    Add-File 'whisper' 'models/whisper-medium/README.md' 'licenses/model-card.md' 'Upstream Whisper model card and license reference'
    foreach ($name in 'translate.exe','translate-gui.exe') { Add-File 'translate' (Join-Path $TranslateBuildDir $name) ('bin/' + $name) 'TranslateGemma application' }
    $contexts = @('prompt-512.gmb.selection.context')
    if (Test-Path -LiteralPath (Join-Path $TranslateBuildDir 'gemma-block/prompt-512.gmb.part-0.bundle.context')) {
        $contexts += 0..2 | ForEach-Object { 'prompt-512.gmb.part-{0}.bundle.context' -f $_ }
    } else { $contexts += 'prompt-512.gmb.bundle.context' }
    foreach ($name in $contexts) {
        Add-File 'translate' (Join-Path $TranslateBuildDir ('gemma-block/' + $name)) ('bin/gemma-block/' + $name) 'Compiled NPU weights/selection graph; restored without Prepare DLL'
    }
    Add-File 'translate' 'models/translategemma-4b-stage4/tokenizer.gta' 'models/translategemma-4b-stage4/tokenizer.gta' 'Native tokenizer'
    Add-File 'translate' 'data/translategemma-4b/README.md' 'licenses/model-card.md' 'Google model card and Gemma terms reference'
    $bindings = @(Read-DistroBindings (Join-Path $TranslateBuildDir 'gemma-block/prompt-512.gmb'))
    $script:distroTranslateBits = $bindings[0].bits
    $relativeBindings = @()
    foreach ($binding in $bindings) {
        $modelRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'models')) + '\'
        if (-not $binding.source.StartsWith($modelRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Binding source is outside models' }
        $name = if ($binding.name -eq 'language_model.model.embed_tokens.weight') { 'embedding.gta' } else { ($binding.name.Split('/')[-1] + '.gta') }
        Add-File 'translate' $binding.source ('models/runtime/' + $name) ('Runtime artifact: ' + $binding.name)
        $relativeBindings += [pscustomobject]@{ name = $binding.name; path = '../../models/runtime/' + $name }
    }
    $plan.Add([pscustomobject]@{ app = 'translate'; source = $null; path = 'bin/gemma-block/prompt-512.gmb'; purpose = 'Relocatable binding: only the five runtime artifacts'; data = (New-DistroBindingBytes -Bindings $relativeBindings -Bits $script:distroTranslateBits) })
    Add-File 'ocr' 'build/QnnHtpPrepare.dll' 'bin/QnnHtpPrepare.dll' 'OCR graph compilation and graph-cache identity'
    foreach ($name in 'ocr-generate.exe','ocr-gui.exe') { Add-File 'ocr' ('build/ocr-app/' + $name) ('bin/ocr-app/' + $name) 'OCR engine/GUI' }
    foreach ($name in @('shared.got','tail.got','large-rope.got') + @(0..23 | ForEach-Object { 'block-{0:00}.got' -f $_ })) {
        Add-File 'ocr' ('models/glm-ocr-vision-v2/' + $name) ('models/glm-ocr-vision-v2/' + $name) 'Vision weights/positional tables'
    }
    foreach ($name in @('embeddings.got','text-shared.got') + @(0..15 | ForEach-Object { 'text-{0:00}.got' -f $_ })) {
        Add-File 'ocr' ('models/glm-ocr-text-v1/' + $name) ('models/glm-ocr-text-v1/' + $name) 'Text decoder weights'
    }
    foreach ($name in @('decode-rope.got','tokenizer.got') + @(0..7 | ForEach-Object { 'head-{0:00}.got' -f $_ })) {
        Add-File 'ocr' ('models/glm-ocr-generation-v2/' + $name) ('models/glm-ocr-generation-v2/' + $name) 'Generation head/tokenizer/positional tables'
    }
    Add-File 'ocr' 'models/glm-ocr/README.md' 'licenses/model-card.md' 'Upstream GLM-OCR model card and MIT license reference'
    return $plan.ToArray()
}

function Get-DistroReadme([string]$App) {
    $usage = switch ($App) {
        'whisper' { @'
# Whisper Medium

From this folder in PowerShell:
    .\bin\whisper.exe --quiet C:\audio\speech.wav

Input must be a 16 kHz, mono, IEEE float32 WAV. Only Medium with the default
m1c (fused,self,logits) offload mode is shipped. Other model/offload switches
need different artifacts and are not part of this distribution. CPU decoder
weights and token bytes accompany the compiled encoder/decoder NPU context.
Audio paths are relative to the caller, model paths to the executable.
'@ }
        'translate' { @'
# TranslateGemma 4B

Open bin\translate-gui.exe, or from this folder in PowerShell:
    .\bin\translate.exe --from de --to en 'Guten Morgen.'

The native W{bits} model uses shared 512-token prefill/decode bundles, a cached
NPU token selector, tokenizer, embedding and four positional tables. Separate
legacy prefill/decode contexts and the other raw weights are not needed.
Binding paths are relative to bin\gemma-block, not the original checkout.
Graph rebuilding/--compile-selection and --padded-decode are development
operations and are not supported by this minimal inference package.
This is an experimental implementation, not a general quality acceptance.
Translation can contain meaning errors and alter markup; verify important output.
'@ }
        'ocr' { @'
# GLM-OCR

Open bin\ocr-app\ocr-gui.exe. Supported inputs are static PNG and 24-bit BMP.
Alternatively, from this folder in PowerShell (create a new empty output folder):
    New-Item -ItemType Directory C:\output\new-run
    .\bin\ocr-app\ocr-generate.exe --generate-text "$PWD\bin\QnnHtp.dll" "$PWD\models\glm-ocr-vision-v2" "$PWD\models\glm-ocr-text-v1" "$PWD\models\glm-ocr-generation-v2" C:\images\page.png C:\output\new-run 256

Replace --generate-text with --generate-formula or --generate-table as needed.
Exit 0 means EOS, 3 means a token/context limit, 1 means failure.
The GUI creates bin\ocr-app\runs; the engine creates bin\ocr-app\graph-cache.
Those generated files are not distribution inputs. The first run compiles
graphs and is slower. Keep this folder writable, outside Program Files.
This retains the existing model/image/context limitations and is not a new
OCR quality acceptance. No reference images or validation fixtures are shipped.
'@ }
    }
    $usage = $usage.Replace('W{bits}', ('W' + $script:distroTranslateBits))
    return $usage + @'


## Platform and contents

Copy this entire application folder, preserving its layout, to Windows 11
ARM64 on a Snapdragon X with a Hexagon V73 NPU and a compatible installed
Qualcomm NPU/FastRPC driver. The SDK, Python, PowerShell scripts, a compiler,
model downloads and this source checkout are not needed for inference.
PowerShell above is only an example shell, not an application dependency.
Windows system DLLs (including Windows UCRT used by the Qualcomm DLLs) and
the OEM NPU/FastRPC driver (libcdsprpc.dll) are prerequisites, not files to
copy out of System32. Newer/different NPU generations are not supported by
this V73 bundle. Cross-machine compatibility is not guaranteed solely by the
Snapdragon brand; compiled QNN contexts must match the runtime/device.

Native application code is statically linked with no CRT/libc. The explicit
third-party runtime exception is Qualcomm QNN: QnnHtp.dll + QnnHtpV73Stub.dll,
libQnnHtpV73Skel.so and libqnnhtpv73.cat. Only OCR additionally needs
QnnHtpPrepare.dll. The matching version and Qualcomm terms are under licenses/.

manifest.json lists every shipped file (except itself), its purpose, byte
count and SHA-256. README, manifest and licenses are documentation/legal
metadata, not inference inputs. No SDK headers, import libraries, development
environments, logs, sample inputs or original training checkpoints are included.
Keep the package at a reasonably short path; the existing translator has a
1024-byte path limit and some model readers use Windows ANSI file APIs.

## Licensing

See licenses/newos.txt, licenses/qnn/ and licenses/model-card.md. Model cards
identify upstream terms; packaging does not grant new redistribution rights.
In particular TranslateGemma is governed by https://ai.google.dev/gemma/terms
and the Gemma prohibited-use policy, not the application source license.
Review the upstream model and Qualcomm redistribution obligations before
publishing these local deployment bundles to others.
'@
}

function Write-Distro([object[]]$Plan, [string]$Destination) {
    $destinationPath = [IO.Path]::GetFullPath($Destination)
    Assert-PlainAncestors $destinationPath
    if (Test-Path -LiteralPath $destinationPath) { throw ('Distribution already exists; choose a new -DistroDir: ' + $destinationPath) }
    $stage = $destinationPath + '.staging-' + [guid]::NewGuid().ToString('N')
    $null = [IO.Directory]::CreateDirectory($stage)
    try {
        foreach ($app in 'whisper','translate','ocr') {
            if ($DistroApp -ne 'all' -and $app -ne $DistroApp) { continue }
            $root = Join-Path $stage $app
            $null = [IO.Directory]::CreateDirectory($root)
            $records = [Collections.Generic.List[object]]::new()
            $seen = @{}
            $entries = @($Plan | Where-Object { $_.app -eq $app })
            $entries += [pscustomobject]@{ path = 'README.md'; source = $null; data = [Text.Encoding]::UTF8.GetBytes((Get-DistroReadme $app)); purpose = 'Usage, contents, prerequisites and limitations' }
            foreach ($entry in $entries) {
                if ($entry.path -notmatch '^[A-Za-z0-9_./-]+$' -or $entry.path -match '(^|/)\.\.?(/|$)|^/|//' -or
                    $entry.path -eq 'manifest.json' -or $seen.ContainsKey($entry.path)) { throw 'Invalid or duplicate distribution path' }
                $seen[$entry.path] = $true
                $output = Join-Path $root $entry.path
                $null = [IO.Directory]::CreateDirectory((Split-Path -Parent $output))
                if ($null -ne $entry.source) {
                    Assert-PlainAncestors $entry.source
                    $before = Get-Digest $entry.source
                    [IO.File]::Copy($entry.source, $output)
                    if ((Get-Digest $output) -ne $before) { throw ('Distribution copy hash mismatch: ' + $entry.path) }
                } else { [IO.File]::WriteAllBytes($output, $entry.data); $before = Get-Digest $output }
                $records.Add([pscustomobject][ordered]@{ path = $entry.path; bytes = ([IO.FileInfo]$output).Length; sha256 = $before; purpose = $entry.purpose })
                Write-Output ('Packaged ' + $app + '/' + $entry.path)
            }
            $manifest = [ordered]@{ version = 1; application = $app; platform = 'Windows ARM64 / Snapdragon X / V73'; qairt = '2.50.0.260828'; files = $records.ToArray() }
            [IO.File]::WriteAllText((Join-Path $root 'manifest.json'), ($manifest | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
            if (@(Get-BuildFiles $root).Count -ne $records.Count + 1) { throw 'Unexpected distribution files' }
            Write-Output ('{0}: {1} files, {2:N2} GiB' -f $app,($records.Count + 1),(($records | Measure-Object bytes -Sum).Sum / 1GB))
        }
        [IO.Directory]::Move($stage, $destinationPath)
        Write-Output ('PASS distribution: ' + $destinationPath)
    } finally {
        if ([IO.Directory]::Exists($stage)) { [IO.Directory]::Delete($stage, $true) }
    }
}

function Test-DistroContract {
    $scratch = Join-Path $repoRoot ('tests/tmp/snapdragon-distro-' + [guid]::NewGuid().ToString('N'))
    $null = [IO.Directory]::CreateDirectory($scratch)
    try {
        $names = @('language_model.model.embed_tokens.weight','fixture/prompt/local-cos','fixture/prompt/local-sin','fixture/prompt/global-cos','fixture/prompt/global-sin')
        $records = @($names | ForEach-Object { [pscustomobject]@{ name = $_; path = '../../models/runtime/test.gta' } })
        $binding = Join-Path $scratch 'binding.gmb'
        [IO.File]::WriteAllBytes($binding, (New-DistroBindingBytes $records))
        if (@(Read-DistroBindings $binding).Count -ne 5) { throw 'Binding round trip failed' }
        [IO.File]::WriteAllBytes($binding, (New-DistroBindingBytes -Bindings $records -Bits 8))
        $roundTrip = @(Read-DistroBindings $binding)
        if ($roundTrip.Count -ne 5 -or @($roundTrip | Where-Object { $_.bits -ne 8 }).Count) { throw 'W8 binding round trip failed' }
        $invalidPrecision = New-DistroBindingBytes $records
        $invalidPrecision[12] = 16
        [IO.File]::WriteAllBytes($binding, $invalidPrecision)
        $rejected = $false
        try { $null = Read-DistroBindings $binding } catch { $rejected = $true }
        if (-not $rejected) { throw 'Unsupported binding precision accepted' }
        [IO.File]::WriteAllBytes($binding, (New-DistroBindingBytes ($records + $records[0])))
        $rejected = $false
        try { $null = Read-DistroBindings $binding } catch { $rejected = $true }
        if (-not $rejected) { throw 'Duplicate runtime binding accepted' }
        [IO.File]::WriteAllBytes($binding, (New-DistroBindingBytes $records[0..3]))
        $rejected = $false
        try { $null = Read-DistroBindings $binding } catch { $rejected = $true }
        if (-not $rejected) { throw 'Missing runtime binding accepted' }
        $plan = @('whisper','translate','ocr' | ForEach-Object { [pscustomobject]@{ app = $_; path = 'bin/test.bin'; source = $binding; data = $null; purpose = 'Synthetic fixture' } })
        $destination = Join-Path $scratch 'copy with spaces'
        Write-Distro $plan $destination
        foreach ($app in 'whisper','translate','ocr') {
            $manifest = ConvertFrom-Json ([IO.File]::ReadAllText((Join-Path $destination ($app + '/manifest.json'))))
            foreach ($record in $manifest.files) {
                if ((Get-Digest (Join-Path $destination ($app + '/' + $record.path))) -ne $record.sha256) { throw 'Manifest digest mismatch' }
            }
        }
        $rejected = $false
        try { Write-Distro $plan $destination } catch { $rejected = $_.Exception.Message -like '*already exists*' }
        if (-not $rejected) { throw 'Existing distribution overwritten' }
        $plan[0].path = '../escape'
        $rejected = $false
        try { Write-Distro $plan (Join-Path $scratch 'invalid') } catch { $rejected = $_.Exception.Message -like '*distribution path*' }
        if (-not $rejected -or (Test-Path (Join-Path $scratch 'invalid'))) { throw 'Unsafe distribution published' }
        Write-Output 'PASS distro contract: binding subset, relative paths, missing/duplicate rejection, copies/hashes, no overwrite, traversal rejection'
    } finally { [IO.Directory]::Delete($scratch, $true) }
}

if ($Target -eq 'test') { Test-CleanContract; return }
if ($Target -eq 'test-distro') { Test-DistroContract; return }
if ($Target -eq 'distro') {
    $DistroDir = [IO.Path]::GetFullPath($DistroDir)
    Assert-PlainAncestors $DistroDir
    if ((Test-Path -LiteralPath $DistroDir) -and -not $WhatIfPreference) { throw ('Distribution already exists; choose a new -DistroDir: ' + $DistroDir) }
    foreach ($protected in @($projectRoot,$buildRoot,(Join-Path $projectRoot 'models'),(Join-Path $projectRoot 'data'))) {
        if ($DistroDir -eq $protected -or $protected.StartsWith($DistroDir.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
            ($protected -ne $projectRoot -and $DistroDir.StartsWith($protected + '\', [StringComparison]::OrdinalIgnoreCase))) { throw 'Unsafe distribution destination' }
    }
    if ($WhatIfPreference) { Get-DistroPlan | Select-Object app,path,purpose | Format-Table -AutoSize }
    if (-not $PSCmdlet.ShouldProcess($DistroDir, 'Rebuild applications and copy minimal self-contained distributions')) { return }
}
Assert-PlainAncestors $buildRoot
$files = @(Get-BuildFiles $buildRoot)
$preserved = @($files | Where-Object { Test-PreservePath (Get-RelativeBuildPath $_.FullName) })
Write-Output ('Build files: {0}; preserve outside build: {1}; disposable: {2}' -f $files.Count,$preserved.Count,($files.Count - $preserved.Count))
Write-Output ('Size: build {0:N2} GiB; preserved inputs/evidence {1:N2} GiB' -f (($files | Measure-Object Length -Sum).Sum / 1GB),(($preserved | Measure-Object Length -Sum).Sum / 1GB))
if ($Target -eq 'status') {
    Write-Output ('Persistent state: ' + $stateRoot)
    Get-ActiveBuildProcesses | Select-Object ProcessId,Name
    return
}
if (-not $PSCmdlet.ShouldProcess($buildRoot, $Target)) { return }
Assert-BuildIdle
Assert-PlainAncestors $stateRoot
if ($Target -in 'build','rebuild','distro') {
    $null = Get-Command $Compiler -ErrorAction Stop
    if (-not ($Target -eq 'distro' -and $DistroApp -eq 'translate') -and
        -not (Test-Path (Join-Path $projectRoot 'data/v2.50.0.260828.zip') -PathType Leaf)) { throw 'Pinned QAIRT archive is required in data/v2.50.0.260828.zip' }
}
New-Item -ItemType Directory -Force $stateRoot | Out-Null
$lock = [IO.File]::Open((Join-Path $stateRoot 'operation.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
$operation = [ordered]@{ target = $Target; started = [DateTime]::UtcNow.ToString('o'); complete = $false; error = $null }
$operationPath = Join-Path $stateRoot 'last-operation.json'
$transcriptStarted = $false
try {
    [IO.File]::WriteAllText($operationPath, ($operation | ConvertTo-Json))
    $null = Start-Transcript -Path (Join-Path $stateRoot 'last-operation.log') -Force
    $transcriptStarted = $true
    if ($Target -in 'clean','rebuild') { Remove-Build }
    if ($Target -eq 'distro' -and $DistroApp -eq 'translate') {
        Push-Location $repoRoot
        try {
            & (Join-Path $projectRoot 'tools/translate/build-gemma.ps1') -Compiler $Compiler -BuildDir $TranslateBuildDir -Translate
            & (Join-Path $projectRoot 'tools/translate/build-gemma.ps1') -Compiler $Compiler -BuildDir $TranslateBuildDir -Gui
        } finally { Pop-Location }
    } elseif ($Target -in 'build','rebuild','distro') { Build-Applications }
    if ($Target -eq 'distro') { Write-Distro @(Get-DistroPlan) $DistroDir }
    $operation.complete = $true
} catch {
    $operation.error = $_.ToString()
    throw
} finally {
    $operation['finished'] = [DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText($operationPath, ($operation | ConvertTo-Json))
    if ($transcriptStarted) { $null = Stop-Transcript }
    $lock.Dispose()
}