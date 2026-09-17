param(
    [Parameter(Mandatory = $true)]
    [string]$WavPath,
    [ValidateSet('tiny', 'base', 'small', 'medium')]
    [string]$Model = 'medium',
    [int]$DecoderWorkers = 0,
    [string]$ProbePath,
    [ValidateSet('cpu', 'all', 'cross', 'mlp', 'self', 'logits',
        'cross,mlp', 'cross,mlp,logits', 'cross,mlp,self',
        'fused', 'fused,logits', 'fused,self', 'fused,self,logits')]
    [string]$DecoderOffload = 'fused,self,logits',
    [ValidateSet('off', 'trace', 'basic', 'detailed')]
    [string]$Diagnostics = 'off',
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$buildDirectory = Join-Path $repoRoot 'experimental\snapdragon\build'
$probe = Join-Path $buildDirectory 'npu_probe.exe'
if ($ProbePath) { $probe = [System.IO.Path]::GetFullPath($ProbePath) }
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

$arguments = "--model=$Model --decoder-offload=$DecoderOffload"
if ($Diagnostics -ne 'off') {
    $diagnosticsPath = Join-Path $OutputDirectory 'diagnostics.csv'
    $arguments += ' --diagnostics=' + $Diagnostics + ' "--diagnostics-output=' + $diagnosticsPath + '"'
}
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
if ($Diagnostics -ne 'off' -and -not ('WhisperThreadSampler' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
public sealed class WhisperThreadSampler : IDisposable {
    public readonly List<string> Rows = new List<string>();
    public int Errors;
    private readonly Process process;
    private readonly Timer timer;
    private readonly object gate = new object();
    public WhisperThreadSampler(Process target) {
        process = target;
        Rows.Add("qpc,thread_id,user_100ns,kernel_100ns,state,wait_reason");
        timer = new Timer(Sample, null, 0, 250);
    }
    private void Sample(object unused) {
        if (!Monitor.TryEnter(gate)) return;
        try {
            process.Refresh();
            if (process.HasExited) return;
            foreach (ProcessThread thread in process.Threads) {
                try {
                    var state = thread.ThreadState;
                    string reason = state == System.Diagnostics.ThreadState.Wait ? thread.WaitReason.ToString() : "";
                    Rows.Add(Stopwatch.GetTimestamp() + "," + thread.Id + "," +
                        thread.UserProcessorTime.Ticks + "," + thread.PrivilegedProcessorTime.Ticks +
                        "," + state + "," + reason);
                } catch { ++Errors; }
                finally { thread.Dispose(); }
            }
        } catch { ++Errors; }
        finally { Monitor.Exit(gate); }
    }
    public void Dispose() {
        using (var finished = new ManualResetEvent(false)) {
            timer.Dispose(finished);
            finished.WaitOne();
        }
    }
}
'@
}
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
if (-not $process.Start()) { throw 'Could not start npu_probe.exe.' }
$sampler = if ($Diagnostics -ne 'off') { [WhisperThreadSampler]::new($process) } else { $null }
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
$process.WaitForExit()
$stopwatch.Stop()
if ($sampler) {
    $sampler.Dispose()
    [System.IO.File]::WriteAllLines((Join-Path $OutputDirectory 'threads.csv'), $sampler.Rows, $utf8NoBom)
}
$exitCode = $process.ExitCode
$cpuSeconds = $process.TotalProcessorTime.TotalSeconds
$stdoutText = $stdoutTask.Result
$stderrText = $stderrTask.Result
$process.Dispose()
[System.IO.File]::WriteAllText($stdoutPath, $stdoutText, $utf8NoBom)
[System.IO.File]::WriteAllText($stderrPath, $stderrText, $utf8NoBom)

$transcriptMatch = [regex]::Match(
    $stdoutText,
    '(?ms)^Full transcript \(German\):\r?\n(.*?)\r?\n  transcribed windows:'
)
if (-not $transcriptMatch.Success) {
    $transcriptMatch = [regex]::Match(
        $stdoutText,
        '(?ms)^Transcript \(German\):\r?\n(.*?)\r?\n  decoder time:'
    )
}
$transcriptSha256 = ''
if ($transcriptMatch.Success) {
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $transcriptBytes = [System.Text.Encoding]::UTF8.GetBytes(
            $transcriptMatch.Groups[1].Value.Trim()
        )
        $transcriptSha256 = [System.BitConverter]::ToString(
            $sha256.ComputeHash($transcriptBytes)
        ).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha256.Dispose()
    }
}

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

function Get-DurationMaximumMs([string]$Label) {
    $pattern = '(?m)^  ' + [regex]::Escape($Label) + ': ([0-9.]+) us\r?$'
    $maximum = 0.0
    foreach ($match in [regex]::Matches($stdoutText, $pattern)) {
        $value = [double]::Parse(
            $match.Groups[1].Value,
            [System.Globalization.CultureInfo]::InvariantCulture
        ) / 1000.0
        if ($value -gt $maximum) { $maximum = $value }
    }
    return $maximum
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
$npuCrossAttentionMs = Get-DurationTotalMs 'NPU cross-attention'
$npuCrossAttentionExecuteMs = Get-DurationTotalMs 'NPU cross-attention graphExecute'
$npuFusedCrossMlpMs = Get-DurationTotalMs 'NPU fused cross/MLP'
$npuFusedCrossMlpExecuteMs = Get-DurationTotalMs 'NPU fused cross/MLP graphExecute'
$npuSelfAttentionMs = Get-DurationTotalMs 'NPU self-attention'
$npuSelfAttentionExecuteMs = Get-DurationTotalMs 'NPU self-attention graphExecute'
$npuFeedForwardMs = Get-DurationTotalMs 'NPU feed-forward'
$npuMlpExecuteMs = Get-DurationTotalMs 'NPU MLP graphExecute'
$npuFinalProjectionMs = Get-DurationTotalMs 'NPU final projection'
$npuFinalProjectionExecuteMs = Get-DurationTotalMs 'NPU final projection graphExecute'
$npuHostCallMs = $frontendMs + $encoderMs + $crossKvNpuMs +
    $npuSelfAttentionExecuteMs + $npuCrossAttentionExecuteMs +
    $npuFusedCrossMlpExecuteMs + $npuMlpExecuteMs + $npuFinalProjectionExecuteMs
$decoderMs = Get-DurationTotalMs 'decoder time'
$selfAttentionMs = Get-DurationTotalMs 'CPU self-attention'
$crossAttentionMs = Get-DurationTotalMs 'CPU cross-attention'
$feedForwardMs = Get-DurationTotalMs 'CPU feed-forward'
$logitsMs = Get-DurationTotalMs 'CPU final norm/logits'
$logMelMs = Get-DurationTotalMs 'WAV to log-mel time'
$windowMs = Get-DurationTotalMs 'window total time'
$contextRestoreMs = Get-DurationTotalMs 'contextCreateFromBinary time'
$nativeCleanupMs = Get-DurationTotalMs 'native cleanup time'
$nativeProcessMs = Get-DurationTotalMs 'native process time'
$generatedTokens = Get-IntegerTotal 'generated tokens'
$decoderSteps = Get-IntegerTotal 'decoder steps'
$minimumDecoderSteps = $generatedTokens + 4L * $windowCount

$summary = [ordered]@{
    model = $Model
    diagnostics = $Diagnostics
    probe_sha256 = (Get-FileHash -LiteralPath $probe -Algorithm SHA256).Hash.ToLowerInvariant()
    wav_sha256 = (Get-FileHash -LiteralPath $WavPath -Algorithm SHA256).Hash.ToLowerInvariant()
    thread_sampling_interval_ms = if ($sampler) { 250 } else { 0 }
    thread_sampling_errors = if ($sampler) { $sampler.Errors } else { 0 }
    decoder_offload = $DecoderOffload
    transcript_sha256 = $transcriptSha256
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
    native_cleanup_ms = $nativeCleanupMs
    native_process_ms = $nativeProcessMs
    log_mel_ms = $logMelMs
    frontend_npu_ms = $frontendMs
    encoder_npu_ms = $encoderMs
    cross_kv_npu_ms = $crossKvNpuMs
    npu_self_attention_ms = $npuSelfAttentionMs
    npu_self_attention_execute_ms = $npuSelfAttentionExecuteMs
    npu_cross_attention_ms = $npuCrossAttentionMs
    npu_cross_attention_execute_ms = $npuCrossAttentionExecuteMs
    npu_fused_cross_mlp_ms = $npuFusedCrossMlpMs
    npu_fused_cross_mlp_execute_ms = $npuFusedCrossMlpExecuteMs
    npu_feed_forward_ms = $npuFeedForwardMs
    npu_mlp_execute_ms = $npuMlpExecuteMs
    npu_final_projection_ms = $npuFinalProjectionMs
    npu_final_projection_execute_ms = $npuFinalProjectionExecuteMs
    npu_self_attention_offload_calls = Get-IntegerTotal 'NPU self-attention offload calls'
    npu_self_attention_graph_submissions = Get-IntegerTotal 'NPU self-attention graph submissions'
    npu_self_attention_maximum_ms = Get-DurationMaximumMs 'NPU self-attention maximum offload'
    npu_self_attention_calls_over_10ms = Get-IntegerTotal 'NPU self-attention calls over 10 ms'
    npu_self_attention_calls_over_100ms = Get-IntegerTotal 'NPU self-attention calls over 100 ms'
    npu_self_attention_calls_over_1000ms = Get-IntegerTotal 'NPU self-attention calls over 1000 ms'
    npu_cross_attention_offload_calls = Get-IntegerTotal 'NPU cross-attention offload calls'
    npu_cross_attention_graph_submissions = Get-IntegerTotal 'NPU cross-attention graph submissions'
    npu_cross_attention_maximum_ms = Get-DurationMaximumMs 'NPU cross-attention maximum offload'
    npu_cross_attention_calls_over_10ms = Get-IntegerTotal 'NPU cross-attention calls over 10 ms'
    npu_cross_attention_calls_over_100ms = Get-IntegerTotal 'NPU cross-attention calls over 100 ms'
    npu_cross_attention_calls_over_1000ms = Get-IntegerTotal 'NPU cross-attention calls over 1000 ms'
    npu_fused_cross_mlp_offload_calls = Get-IntegerTotal 'NPU fused cross/MLP offload calls'
    npu_fused_cross_mlp_graph_submissions = Get-IntegerTotal 'NPU fused cross/MLP graph submissions'
    npu_fused_cross_mlp_maximum_ms = Get-DurationMaximumMs 'NPU fused cross/MLP maximum offload'
    npu_fused_cross_mlp_calls_over_10ms = Get-IntegerTotal 'NPU fused cross/MLP calls over 10 ms'
    npu_fused_cross_mlp_calls_over_100ms = Get-IntegerTotal 'NPU fused cross/MLP calls over 100 ms'
    npu_fused_cross_mlp_calls_over_1000ms = Get-IntegerTotal 'NPU fused cross/MLP calls over 1000 ms'
    npu_mlp_offload_calls = Get-IntegerTotal 'NPU MLP offload calls'
    npu_mlp_graph_submissions = Get-IntegerTotal 'NPU MLP graph submissions'
    npu_mlp_maximum_ms = Get-DurationMaximumMs 'NPU MLP maximum offload'
    npu_mlp_calls_over_10ms = Get-IntegerTotal 'NPU MLP calls over 10 ms'
    npu_mlp_calls_over_100ms = Get-IntegerTotal 'NPU MLP calls over 100 ms'
    npu_mlp_calls_over_1000ms = Get-IntegerTotal 'NPU MLP calls over 1000 ms'
    npu_final_projection_offload_calls = Get-IntegerTotal 'NPU final projection offload calls'
    npu_final_projection_graph_submissions = Get-IntegerTotal 'NPU final projection graph submissions'
    npu_final_projection_maximum_ms = Get-DurationMaximumMs 'NPU final projection maximum offload'
    npu_final_projection_calls_over_10ms = Get-IntegerTotal 'NPU final projection calls over 10 ms'
    npu_final_projection_calls_over_100ms = Get-IntegerTotal 'NPU final projection calls over 100 ms'
    npu_final_projection_calls_over_1000ms = Get-IntegerTotal 'NPU final projection calls over 1000 ms'
    decoder_ms = $decoderMs
    self_attention_ms = $selfAttentionMs
    cross_attention_ms = $crossAttentionMs
    feed_forward_ms = $feedForwardMs
    final_norm_logits_ms = $logitsMs
    window_total_ms = $windowMs
    generated_tokens = $generatedTokens
    decoder_steps = $decoderSteps
    prefix_reused_steps = Get-IntegerTotal 'decoder prefix reused steps'
    transformer_steps = $decoderSteps - (Get-IntegerTotal 'decoder prefix reused steps')
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