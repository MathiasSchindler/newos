[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidateSet('build','clean','rebuild','status','test')][string]$Target = 'build',
    [string]$Compiler = 'clang'
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$buildRoot = Join-Path $projectRoot 'build'
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
    return (Test-DevelopmentPath $Path) -or $Path -match '^gemma-block/prompt-512\.gmb(?:\.[a-z.]+)?$|^[^/]+\.qnnctx$|^ocr-app/(graph-cache/|runs/|[^/]+\.(png|bmp)$)'
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
        foreach ($name in @('translate.exe','old.obj','unknown-model.bin','gemma-block/prompt-512.gmb','gemma-block/prompt-512.gmb.bundle.context','calibration-venv/Scripts/python.exe','calibration-venv/__pycache__/generated.pyc','ocr-app/runs/input.png','ocr-app/graph-cache/2-00.qob','obsolete/2-00.qob','evidence/report.json',$longPython)) {
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
        if ($state.Count -ne 8 -or -not $state.ContainsKey('unknown-model.bin')) { throw 'Preservation classification failed' }
        Restore-BuildState
        if ((Get-BuildFiles $buildRoot | Measure-Object).Count -ne 6 -or (Test-Path (Join-Path $buildRoot 'translate.exe'))) { throw 'Restore included disposable output or omitted input' }
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

if ($Target -eq 'test') { Test-CleanContract; return }
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
if ($Target -in 'build','rebuild') {
    $null = Get-Command $Compiler -ErrorAction Stop
    if (-not (Test-Path (Join-Path $projectRoot 'data/v2.50.0.260828.zip') -PathType Leaf)) { throw 'Pinned QAIRT archive is required in data/v2.50.0.260828.zip' }
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
    if ($Target -in 'build','rebuild') { Build-Applications }
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