param(
    [Parameter(Mandatory = $true)]
    [string]$WavPath,
    [ValidateSet('tiny', 'base', 'small')]
    [string]$Model = 'small',
    [int]$DecoderWorkers = 0,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$buildDirectory = Join-Path $repoRoot 'experimental\snapdragon\build'
$probe = Join-Path $buildDirectory 'npu_probe.exe'
$WavPath = [System.IO.Path]::GetFullPath($WavPath)
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot "experimental\snapdragon\data\profile-$Model"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$stdoutPath = Join-Path $OutputDirectory "$Model.stdout.txt"
$stderrPath = Join-Path $OutputDirectory "$Model.stderr.txt"
$summaryPath = Join-Path $OutputDirectory "$Model-summary.txt"
$jsonPath = Join-Path $OutputDirectory "$Model-summary.json"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

if (-not (Test-Path -LiteralPath $probe)) { throw "Missing probe: $probe" }
if (-not (Test-Path -LiteralPath $WavPath)) { throw "Missing WAV: $WavPath" }
if ($WavPath.Contains('"')) { throw 'WAV paths containing quotes are not supported.' }
if ($DecoderWorkers -lt 0 -or $DecoderWorkers -gt 32) {
    throw 'DecoderWorkers must be zero for automatic selection or between 1 and 32.'
}
if (-not (Get-Command ffprobe -ErrorAction SilentlyContinue)) {
    throw 'ffprobe was not found in PATH.'
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$durationText = & ffprobe -v error -show_entries format=duration `
    -of default=noprint_wrappers=1:nokey=1 $WavPath
if ($LASTEXITCODE -ne 0) { throw "ffprobe failed with exit code $LASTEXITCODE." }
$audioSeconds = [double]::Parse(
    $durationText.Trim(), [System.Globalization.CultureInfo]::InvariantCulture
)

$arguments = "--model=$Model"
if ($DecoderWorkers -gt 0) { $arguments += " --decoder-workers=$DecoderWorkers" }
$arguments += ' "' + $WavPath + '"'
$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $probe
$startInfo.Arguments = $arguments
$startInfo.WorkingDirectory = $buildDirectory
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$startInfo.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$startInfo.StandardErrorEncoding = [System.Text.Encoding]::UTF8
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $startInfo
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
if (-not $process.Start()) { throw 'Could not start npu_probe.exe.' }
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
$process.WaitForExit()
$stopwatch.Stop()
$exitCode = $process.ExitCode
$cpuSeconds = $process.TotalProcessorTime.TotalSeconds
$stdoutText = $stdoutTask.Result
$stderrText = $stderrTask.Result
$process.Dispose()
[System.IO.File]::WriteAllText($stdoutPath, $stdoutText, $utf8NoBom)
[System.IO.File]::WriteAllText($stderrPath, $stderrText, $utf8NoBom)

function Get-DurationTotalMs([string]$Label) {
    $pattern = '(?m)^  ' + [regex]::Escape($Label) + ': ([0-9.]+) us\r?$'
    $total = 0.0
    foreach ($match in [regex]::Matches($stdoutText, $pattern)) {
        $total += [double]::Parse(
            $match.Groups[1].Value,
            [System.Globalization.CultureInfo]::InvariantCulture
        ) / 1000.0
    }
    return $total
}

function Get-IntegerTotal([string]$Label) {
    $pattern = '(?m)^  ' + [regex]::Escape($Label) + ': ([0-9]+)\r?$'
    $total = 0L
    foreach ($match in [regex]::Matches($stdoutText, $pattern)) {
        $total += [long]$match.Groups[1].Value
    }
    return $total
}

function Get-IntegerMaximum([string]$Label) {
    $pattern = '(?m)^  ' + [regex]::Escape($Label) + ': ([0-9]+)\r?$'
    $maximum = 0L
    foreach ($match in [regex]::Matches($stdoutText, $pattern)) {
        $value = [long]$match.Groups[1].Value
        if ($value -gt $maximum) { $maximum = $value }
    }
    return $maximum
}

$wallSeconds = $stopwatch.Elapsed.TotalSeconds
$logicalProcessors = [Environment]::ProcessorCount
$equivalentCpuCores = if ($wallSeconds -gt 0.0) { $cpuSeconds / $wallSeconds } else { 0.0 }
$cpuCapacityPercent = 100.0 * $equivalentCpuCores / $logicalProcessors
$windowCount = [regex]::Matches(
    $stdoutText, '(?m)^=== WHISPER BATCH SEGMENT [0-9]+ BEGIN ===\r?$'
).Count
$frontendMs = Get-DurationTotalMs 'cached frontend NPU time'
$encoderMs = Get-DurationTotalMs 'cached encoder NPU time'
$crossKvNpuMs = Get-DurationTotalMs 'decoder cross K/V NPU time'
$npuFeedForwardMs = Get-DurationTotalMs 'NPU feed-forward'
$npuMlpExecuteMs = Get-DurationTotalMs 'NPU MLP graphExecute'
$npuHostCallMs = $frontendMs + $encoderMs + $crossKvNpuMs + $npuMlpExecuteMs
$decoderMs = Get-DurationTotalMs 'decoder time'
$selfAttentionMs = Get-DurationTotalMs 'CPU self-attention'
$crossAttentionMs = Get-DurationTotalMs 'CPU cross-attention'
$feedForwardMs = Get-DurationTotalMs 'CPU feed-forward'
$logitsMs = Get-DurationTotalMs 'CPU final norm/logits'
$logMelMs = Get-DurationTotalMs 'WAV to log-mel time'
$windowMs = Get-DurationTotalMs 'window total time'
$contextRestoreMs = Get-DurationTotalMs 'contextCreateFromBinary time'
$generatedTokens = Get-IntegerTotal 'generated tokens'
$decoderSteps = Get-IntegerTotal 'decoder steps'
$minimumDecoderSteps = $generatedTokens + 4L * $windowCount

$summary = [ordered]@{
    model = $Model
    wav = $WavPath
    audio_seconds = $audioSeconds
    windows = $windowCount
    exit_code = $exitCode
    wall_seconds = $wallSeconds
    realtime_factor = if ($audioSeconds -gt 0.0) { $wallSeconds / $audioSeconds } else { 0.0 }
    audio_x_realtime = if ($wallSeconds -gt 0.0) { $audioSeconds / $wallSeconds } else { 0.0 }
    process_cpu_seconds = $cpuSeconds
    equivalent_cpu_cores = $equivalentCpuCores
    cpu_total_capacity_percent = $cpuCapacityPercent
    logical_processors = $logicalProcessors
    npu_host_call_ms = $npuHostCallMs
    npu_host_call_duty_percent = if ($wallSeconds -gt 0.0) {
        100.0 * $npuHostCallMs / ($wallSeconds * 1000.0)
    } else { 0.0 }
    context_restore_ms = $contextRestoreMs
    log_mel_ms = $logMelMs
    frontend_npu_ms = $frontendMs
    encoder_npu_ms = $encoderMs
    cross_kv_npu_ms = $crossKvNpuMs
    npu_feed_forward_ms = $npuFeedForwardMs
    npu_mlp_execute_ms = $npuMlpExecuteMs
    decoder_ms = $decoderMs
    self_attention_ms = $selfAttentionMs
    cross_attention_ms = $crossAttentionMs
    feed_forward_ms = $feedForwardMs
    final_norm_logits_ms = $logitsMs
    window_total_ms = $windowMs
    generated_tokens = $generatedTokens
    decoder_steps = $decoderSteps
    retry_steps = [math]::Max(0L, $decoderSteps - $minimumDecoderSteps)
    peak_resident_bytes = Get-IntegerMaximum 'process peak resident bytes'
    peak_private_committed_bytes = Get-IntegerMaximum 'process private committed bytes'
    stderr_bytes = [System.Text.Encoding]::UTF8.GetByteCount($stderrText)
    npu_hardware_occupancy_available = $false
}

$summaryLines = foreach ($entry in $summary.GetEnumerator()) {
    if ($entry.Value -is [double]) {
        '{0}={1}' -f $entry.Key, $entry.Value.ToString(
            'F6', [System.Globalization.CultureInfo]::InvariantCulture
        )
    } elseif ($entry.Value -is [bool]) {
        '{0}={1}' -f $entry.Key, $entry.Value.ToString().ToLowerInvariant()
    } else {
        '{0}={1}' -f $entry.Key, $entry.Value
    }
}
[System.IO.File]::WriteAllText($summaryPath, ($summaryLines -join "`r`n") + "`r`n", $utf8NoBom)
[System.IO.File]::WriteAllText(
    $jsonPath, ($summary | ConvertTo-Json) + "`r`n", $utf8NoBom
)
$summaryLines
if ($exitCode -ne 0) { throw "npu_probe.exe failed with exit code $exitCode." }