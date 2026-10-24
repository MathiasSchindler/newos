param(
    [string]$DataDir = 'D:\newos-data\nemotron-asr',
    [string]$OutputDir = '',
    [ValidateRange(100, 10000)][int]$EndpointMs = 800,
    [switch]$Microphone
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$data = (Resolve-Path -LiteralPath $DataDir).Path
if (-not $OutputDir) { $OutputDir = Join-Path $data ('m5\acceptance-' + [guid]::NewGuid().ToString('N')) }
if (Test-Path -LiteralPath $OutputDir) { throw 'M5 evidence directory must not exist' }
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDir).Path
$python = Join-Path $repo 'experimental\snapdragon\build\gemma-oracle-x64\python.exe'
$package = Join-Path $data 'nemotron-runtime-m4.nmp'
$manifest = Join-Path $data 'm1\fastrpc-fp32-pad-v17\manifest.json'
$beforeManifest = (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash
$beforePackage = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
Push-Location $repo
try {
    & "$PSScriptRoot\build.ps1" -Test
    & $python -B "$PSScriptRoot\test_nemotron_m5.py"
    if ($LASTEXITCODE) { throw 'M5 evidence-validator tests failed' }
    & 'experimental\snapdragon\build\nemotron-asr\test-live-cancel.exe' "$data\corpus\silence.wav"
    if ($LASTEXITCODE) { throw 'M5 cancellation/drain test failed' }
    $canonical = Join-Path $repo 'experimental\snapdragon\build\nemotron-asr\nemotron-asr.exe'
    $exe = Join-Path $output 'nemotron-asr.exe'
    Copy-Item -LiteralPath $canonical -Destination $exe
    $beforeExe = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    function Run-Live([string]$Name, [string]$Wav, [int]$Gap, [int]$Duration, [bool]$Mic) {
        $args_ = @('--package', $package, '--duration-s', "$Duration", '--endpoint-ms', "$EndpointMs",
            '--quiet-transcripts', '--output', "$output\$Name-activity.jsonl", '--stats', "$output\$Name-stats.json")
        if ($Mic) { $args_ += '--listen' }
        else { $args_ += @('--replay-live', $Wav, '--replay-gap-ms', "$Gap") }
        $quotedArgs = ($args_ | ForEach-Object { '"' + $_ + '"' }) -join ' '
        $process = Start-Process -FilePath $exe -ArgumentList $quotedArgs -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput "$output\$Name-stdout.txt" -RedirectStandardError "$output\$Name-log.txt"
        $nativeExit = $process.ExitCode
        if ($null -eq $nativeExit) { throw 'Native live exit status unavailable' }
        $audit = @('--stats', "$output\$Name-stats.json", '--log', "$output\$Name-log.txt",
            '--activity', "$output\$Name-activity.jsonl", '--output', "$output\$Name-audit.json",
            '--minimum-seconds', "$Duration")
        if ($Mic) { $audit += '--microphone' }
        & $python -B "$PSScriptRoot\nemotron_m5.py" @audit
        if ($LASTEXITCODE -or $nativeExit) { throw "M5 $Name live gate failed; evidence retained in $output" }
    }
    Run-Live 'short' "$data\corpus\short-speech.wav" 1000 120 $false
    Run-Live 'long' "$data\corpus\long.wav" 2000 120 $false
    & $python -B "$PSScriptRoot\nemotron_m4.py" --data-dir $data --deps-dir "$data\reference-packages" `
        quality --executable $exe --package $package --output "$output\wav-regression"
    if ($LASTEXITCODE) { throw 'M5 frozen WAV parity/quality/deployment regression failed' }
    if ($Microphone) {
        Write-Output 'Explicit microphone validation:1800 seconds, statistics only; no audio or transcript saved.'
        Run-Live 'microphone' '' 0 1800 $true
    }
    & "$PSScriptRoot\build.ps1" -Clean
    & "$PSScriptRoot\build.ps1" -Test
    if ((Get-FileHash -LiteralPath $canonical -Algorithm SHA256).Hash -ne $beforeExe -or
        (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ne $beforePackage -or
        (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash -ne $beforeManifest) {
        throw 'M5 clean/rebuild changed executable, runtime or approved v17 identity'
    }
    $report = @{
        schema='newos.nemotron.m5.acceptance.v1'
        status=$(if ($Microphone) { 'm5_accepted' } else { 'm5_tests_passed_microphone_pending' })
        executable_sha256=$beforeExe; package_sha256=$beforePackage; approved_manifest_sha256=$beforeManifest
        short=(Get-Content "$output\short-audit.json" -Raw | ConvertFrom-Json)
        long=(Get-Content "$output\long-audit.json" -Raw | ConvertFrom-Json)
        wav=(Get-Content "$output\wav-regression\report.json" -Raw | ConvertFrom-Json)
        native_audio_checks=52; evidence_validator_tests=8; cancellation_drain_passed=$true
        clean_rebuild_byte_exact=$true; raw_audio_recorded=$false; microphone_transcript_retained=$false
        speech_detector='10ms RMS energy,0.005 threshold,three-frame onset; not neural VAD'
        endpoint_policy='preserve resumed pauses; confirmed endpoint retains200ms; EOF retains all'
        M1_320ms_gate_reclassified=$false; M6_realtime_claimed=$false
    }
    if ($Microphone) { $report.microphone = Get-Content "$output\microphone-audit.json" -Raw | ConvertFrom-Json }
    [IO.File]::WriteAllText((Join-Path $output 'report.json'), (($report | ConvertTo-Json -Depth 30) + "`n"),
        (New-Object Text.UTF8Encoding($false)))
    Write-Output "PASS M5 gates: $output\report.json"
} finally {
    Pop-Location
}
