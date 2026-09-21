param(
    [string]$DistroDir = '',
    [string]$ReportDir = '',
    [switch]$TranslateOnly
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $DistroDir) { $DistroDir = Join-Path $projectRoot 'distro' }
if (-not $ReportDir) { $ReportDir = Join-Path $projectRoot ('data/distro-verification-' + [guid]::NewGuid().ToString('N')) }
$DistroDir = (Resolve-Path -LiteralPath $DistroDir).Path
$ReportDir = [IO.Path]::GetFullPath($ReportDir)
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('Snapdragon distro ' + [guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($scratch)
$null = [IO.Directory]::CreateDirectory($ReportDir)
$working = Join-Path $scratch 'empty working directory'
$null = [IO.Directory]::CreateDirectory($working)
$results = [Collections.Generic.List[object]]::new()
$complete = $false

function Invoke-Native([string]$Name, [string]$Executable, [string[]]$Arguments, [int[]]$Expected = @(0)) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.WorkingDirectory = $working
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $start.StandardErrorEncoding = [Text.Encoding]::UTF8
    $start.Arguments = (@($Arguments | ForEach-Object {
        if ($_.Contains('"') -or $_.EndsWith('\')) { throw 'Unsupported test argument' }
        '"' + $_ + '"'
    }) -join ' ')
    $start.EnvironmentVariables['PATH'] = (Join-Path $env:SystemRoot 'System32')
    foreach ($environmentName in @($start.EnvironmentVariables.Keys)) {
        if ($environmentName -match 'QNN|QAIRT|ADSP|HEXAGON') { $start.EnvironmentVariables.Remove($environmentName) }
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        Write-Output ('Running ' + $Name)
        if (-not $process.Start()) { throw ('Cannot launch ' + $Name) }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(1200000)) { $process.Kill(); $process.WaitForExit(); throw ('Timeout: ' + $Name) }
        $output = $stdout.Result; $errorOutput = $stderr.Result
        [IO.File]::WriteAllText((Join-Path $ReportDir ($Name + '.stdout.txt')), $output, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $ReportDir ($Name + '.stderr.txt')), $errorOutput, [Text.UTF8Encoding]::new($false))
        $results.Add([pscustomobject]@{ name = $Name; exit = $process.ExitCode; seconds = $timer.Elapsed.TotalSeconds })
        if ($process.ExitCode -notin $Expected) { throw ('Unexpected exit {0} from {1}; see {2}' -f $process.ExitCode,$Name,$ReportDir) }
        Write-Output ('PASS {0}: exit {1}, {2:N2}s' -f $Name,$process.ExitCode,$timer.Elapsed.TotalSeconds)
    } finally { $process.Dispose() }
}

try {
    foreach ($app in 'whisper','translate','ocr') {
        if ($TranslateOnly -and $app -ne 'translate') { continue }
        $source = Join-Path $DistroDir $app
        $destination = Join-Path $scratch $app
        $manifest = ConvertFrom-Json ([IO.File]::ReadAllText((Join-Path $source 'manifest.json')))
        if ($manifest.version -ne 1 -or $manifest.application -ne $app) { throw 'Invalid package manifest' }
        $seen = @{}
        foreach ($record in $manifest.files) {
            if ($record.path -notmatch '^[A-Za-z0-9_./-]+$' -or $record.path -match '(^|/)\.\.?(/|$)|^/|//' -or
                $record.path -eq 'manifest.json' -or $seen.ContainsKey($record.path)) { throw 'Invalid manifest path' }
            $seen[$record.path] = $true
            $inputFile = Join-Path $source $record.path
            $outputFile = Join-Path $destination $record.path
            $null = [IO.Directory]::CreateDirectory((Split-Path -Parent $outputFile))
            [IO.File]::Copy($inputFile, $outputFile)
            if (([IO.FileInfo]$outputFile).Length -ne $record.bytes -or
                (Get-FileHash -LiteralPath $outputFile -Algorithm SHA256).Hash -ne $record.sha256) { throw ('Manifest mismatch: ' + $inputFile) }
        }
        if (@(Get-ChildItem -LiteralPath $source -Recurse -File).Count -ne $seen.Count + 1) { throw ('Unlisted files in ' + $source) }
        $dlls = @(Get-ChildItem -LiteralPath (Join-Path $destination 'bin') -Filter '*.dll' -File | Select-Object -ExpandProperty Name)
        $expectedDlls = @('QnnHtp.dll','QnnHtpV73Stub.dll')
        if ($app -eq 'ocr') { $expectedDlls += 'QnnHtpPrepare.dll' }
        if (@(Compare-Object $dlls $expectedDlls).Count) { throw ('Unexpected QNN DLL set: ' + $app) }
        Write-Output ('PASS relocated manifest and minimal DLL set: ' + $app)
    }
    if (-not $TranslateOnly) {
    $wav = Join-Path $working 'speech.wav'
    [IO.File]::Copy((Join-Path $projectRoot 'data/long-form-35s.wav'), $wav)
    $image = Join-Path $working 'receipt.png'
    [IO.File]::Copy((Join-Path $projectRoot 'models/glm-ocr-vision-v2/receipt.png'), $image)

    $whisper = Join-Path $scratch 'whisper/bin/whisper.exe'
    Invoke-Native 'whisper' $whisper @('speech.wav')
    $whisperOutput = [IO.File]::ReadAllText((Join-Path $ReportDir 'whisper.stdout.txt'))
    if ($whisperOutput -notmatch 'contextCreateFromBinary.*0x0000000000000000' -or $whisperOutput -notmatch 'transcript') { throw 'Whisper did not restore and transcribe' }
    $context = Join-Path $scratch 'whisper/bin/whisper-medium-m1c-self-fused-v1-encoder-fp16-l24.qnnctx'
    [IO.File]::Move($context, ($context + '.hidden'))
    try { Invoke-Native 'whisper-missing-context' $whisper @('--quiet','speech.wav') @(107) }
    finally { [IO.File]::Move(($context + '.hidden'), $context) }
    }

    $translate = Join-Path $scratch 'translate/bin/translate.exe'
    Invoke-Native 'translate' $translate @('--quiet','--from','de','--to','en','Guten Morgen.')
    if ([IO.File]::ReadAllText((Join-Path $ReportDir 'translate.stdout.txt')).Trim() -cne 'Good morning.') { throw 'Unexpected greeting translation' }
    $embedding = Join-Path $scratch 'translate/models/runtime/embedding.gta'
    [IO.File]::Move($embedding, ($embedding + '.hidden'))
    try { Invoke-Native 'translate-missing-embedding' $translate @('--quiet','--from','de','--to','en','Guten Morgen.') @(1) }
    finally { [IO.File]::Move(($embedding + '.hidden'), $embedding) }
    if ($TranslateOnly) {
        Invoke-Native 'translate-parity' $translate @('--from','de','--to','en','--max-tokens','16','--verify-decode','Guten Morgen.')
        $diagnostics = [IO.File]::ReadAllText((Join-Path $ReportDir 'translate-parity.stderr.txt'))
        if ($diagnostics -notmatch 'PASS decode KV/logits parity position:' -or $diagnostics -notmatch 'cleanup errors: 0') { throw 'Missing decode parity or cleanup evidence' }
        $binding = Join-Path $scratch 'translate/bin/gemma-block/prompt-512.gmb'
        $partition = $binding + '.part-2.bundle.context'
        if (Test-Path -LiteralPath $partition) {
            if ($diagnostics -notmatch 'TranslateGemma 4B W8' -or ([regex]::Matches($diagnostics, 'bundle restore: 0')).Count -ne 3) { throw 'W8 partitions did not restore' }
            [IO.File]::Move($partition, ($partition + '.hidden'))
            try { Invoke-Native 'translate-missing-partition' $translate @('--from','de','--to','en','Guten Morgen.') @(1) }
            finally { [IO.File]::Move(($partition + '.hidden'), $partition) }
            foreach ($mutation in @(@{ name = 'precision'; path = $binding; offset = 12; value = 4 },
                                     @{ name = 'context-header'; path = ($binding + '.part-0.bundle.context'); offset = 0; value = 0 })) {
                $stream = [IO.File]::Open($mutation.path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
                try { $stream.Position = $mutation.offset; $original = $stream.ReadByte(); $stream.Position = $mutation.offset; $stream.WriteByte($mutation.value) }
                finally { $stream.Dispose() }
                try { Invoke-Native ('translate-invalid-' + $mutation.name) $translate @('--from','de','--to','en','Guten Morgen.') @(1) }
                finally {
                    $stream = [IO.File]::OpenWrite($mutation.path)
                    try { $stream.Position = $mutation.offset; $stream.WriteByte($original) } finally { $stream.Dispose() }
                }
            }
        }
    }

    if (-not $TranslateOnly) {
    $ocr = Join-Path $scratch 'ocr/bin/ocr-app/ocr-generate.exe'
    Invoke-Native 'ocr-self-test' $ocr @('--self-test')
    $capture = Join-Path $scratch 'ocr-output'
    $null = [IO.Directory]::CreateDirectory($capture)
    Invoke-Native 'ocr-generate' $ocr @('--generate-text',(Join-Path $scratch 'ocr/bin/QnnHtp.dll'),
        (Join-Path $scratch 'ocr/models/glm-ocr-vision-v2'),(Join-Path $scratch 'ocr/models/glm-ocr-text-v1'),
        (Join-Path $scratch 'ocr/models/glm-ocr-generation-v2'),$image,$capture,'32') @(0,3)
    $generated = @(Get-ChildItem -LiteralPath $capture -Filter '*.generated.txt' -File)
    if ($generated.Count -ne 1 -or $generated[0].Length -eq 0) { throw 'Missing OCR generated text' }
    Copy-Item -LiteralPath $generated[0].FullName -Destination (Join-Path $ReportDir 'ocr-generated.txt')
    }
    $guiMode = if ($TranslateOnly) { '--distro-translate' } else { '--distro' }
    Invoke-Native 'guis' (Join-Path $projectRoot 'build/calibration-venv/Scripts/python.exe') @(
        (Join-Path $PSScriptRoot 'test-translate-gui.py'),$guiMode,$scratch,$ReportDir)
    $complete = $true
    Write-Output ('PASS isolated NPU inference and missing-asset rejection; report: ' + $ReportDir)
} finally {
    [IO.File]::WriteAllText((Join-Path $ReportDir 'results.json'), ([ordered]@{ complete = $complete; distro = $DistroDir; relocated = $scratch; checks = $results.ToArray() } | ConvertTo-Json -Depth 5))
    [IO.Directory]::Delete($scratch, $true)
}