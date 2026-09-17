param(
    [string]$InputMp3,
    [string]$FullWav,
    [string]$OutputDirectory,
    [double]$WindowSeconds = 30.0,
    [double]$OverlapSeconds = 5.0,
    [int]$MaximumSegments = 0,
    [ValidateSet('tiny', 'base', 'small', 'medium')]
    [string]$Model = 'small',
    [ValidateSet('cpu', 'all', 'cross', 'mlp', 'self', 'logits',
        'cross,mlp', 'cross,mlp,logits', 'cross,mlp,self',
        'fused', 'fused,logits', 'fused,self', 'fused,self,logits')]
    [string]$DecoderOffload = 'cross,mlp',
    [switch]$SkipConversion,
    [switch]$Resume,
    [switch]$RetryFlagged,
    [switch]$PerWindowProcesses,
    [switch]$KeepWindows
)

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$snapdragonRoot = Join-Path $repoRoot 'experimental\snapdragon'
if (-not $InputMp3) {
    $InputMp3 = Join-Path $snapdragonRoot 'data\7654653_mp3_128kb_stereo_de_128.mp3'
}
if (-not $FullWav) {
    $FullWav = Join-Path $snapdragonRoot 'data\bundestag-hearing-16k-mono-f32.wav'
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $snapdragonRoot 'data\bundestag-hearing-transcription'
}
$InputMp3 = [System.IO.Path]::GetFullPath($InputMp3)
$FullWav = [System.IO.Path]::GetFullPath($FullWav)
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$buildDirectory = Join-Path $snapdragonRoot 'build'
$probe = Join-Path $buildDirectory 'npu_probe.exe'
$windowDirectory = Join-Path $OutputDirectory 'windows'
$logDirectory = Join-Path $OutputDirectory 'logs'
$resultDirectory = Join-Path $OutputDirectory 'results'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

if ($WindowSeconds -le 0.0) { throw 'WindowSeconds must be positive.' }
if ($OverlapSeconds -lt 0.0 -or $OverlapSeconds -ge $WindowSeconds) {
    throw 'OverlapSeconds must be non-negative and smaller than WindowSeconds.'
}
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) { throw 'ffmpeg was not found in PATH.' }
if (-not (Get-Command ffprobe -ErrorAction SilentlyContinue)) { throw 'ffprobe was not found in PATH.' }
if (-not (Test-Path $probe)) { throw "Missing probe executable: $probe" }

New-Item -ItemType Directory -Force -Path $OutputDirectory, $windowDirectory, $logDirectory, $resultDirectory | Out-Null

function Write-Utf8File([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Invoke-Probe([string]$WavPath, [string]$StdoutPath, [string]$StderrPath) {
    if ($WavPath.Contains('"')) { throw 'WAV paths containing quotes are not supported.' }
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $probe
    $startInfo.Arguments = "--model=$Model --decoder-offload=$DecoderOffload `"$WavPath`""
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
    $stdoutText = $stdoutTask.Result
    $stderrText = $stderrTask.Result
    $process.Dispose()
    Write-Utf8File $StdoutPath $stdoutText
    Write-Utf8File $StderrPath $stderrText
    return [pscustomobject]@{
        ExitCode = $exitCode
        WallMilliseconds = $stopwatch.Elapsed.TotalMilliseconds
    }
}

function Split-BatchOutput([string]$Text) {
    $blocks = @{}
    $lines = $Text -split "`r?`n"
    $currentIndex = -1
    $currentLines = New-Object System.Collections.Generic.List[string]
    foreach ($line in $lines) {
        if ($line -match '^=== WHISPER BATCH SEGMENT ([0-9]+) BEGIN ===$') {
            if ($currentIndex -ge 0) { throw 'Nested persistent batch segment marker.' }
            $currentIndex = [int]$matches[1]
            $currentLines.Clear()
        } elseif ($line -match '^=== WHISPER BATCH SEGMENT ([0-9]+) END ===$') {
            $endIndex = [int]$matches[1]
            if ($currentIndex -ne $endIndex) { throw 'Mismatched persistent batch segment marker.' }
            $blocks[$currentIndex] = ($currentLines -join "`r`n") + "`r`n"
            $currentIndex = -1
            $currentLines.Clear()
        } elseif ($currentIndex -ge 0) {
            $currentLines.Add($line)
        }
    }
    if ($currentIndex -ge 0) { throw 'Unterminated persistent batch segment marker.' }
    return $blocks
}

function Format-Timestamp([double]$Seconds) {
    $wholeSeconds = [long][math]::Floor($Seconds)
    $hours = [long][math]::Floor($wholeSeconds / 3600)
    $minutes = [long][math]::Floor(($wholeSeconds % 3600) / 60)
    $seconds = $wholeSeconds % 60
    return '{0:00}:{1:00}:{2:00}' -f $hours, $minutes, $seconds
}

function Get-NormalizedWords([string[]]$Words) {
    return @($Words | ForEach-Object { ($_ -replace '[^\p{L}\p{N}]', '').ToLowerInvariant() })
}

function Get-OverlapWordCount([string[]]$ExistingWords, [string[]]$NewWords) {
    if ($ExistingWords.Count -eq 0 -or $NewWords.Count -eq 0) { return 0 }
    $maximum = [math]::Min(50, [math]::Min($ExistingWords.Count, $NewWords.Count))
    $existingNormalized = Get-NormalizedWords $ExistingWords
    $newNormalized = Get-NormalizedWords $NewWords
    for ($count = $maximum; $count -ge 3; --$count) {
        $matches = $true
        for ($index = 0; $index -lt $count; ++$index) {
            if ($existingNormalized[$ExistingWords.Count - $count + $index] -ne $newNormalized[$index]) {
                $matches = $false
                break
            }
        }
        if ($matches) { return $count }
    }
    $existingStart = [math]::Max(0, $ExistingWords.Count - 50)
    $newLimit = [math]::Min(50, $NewWords.Count)
    $bestLength = 0
    $bestNewEnd = 0
    for ($existingIndex = $existingStart; $existingIndex -lt $ExistingWords.Count; ++$existingIndex) {
        for ($newIndex = 0; $newIndex -lt $newLimit; ++$newIndex) {
            $length = 0
            while ($existingIndex + $length -lt $ExistingWords.Count -and
                $newIndex + $length -lt $newLimit -and
                $existingNormalized[$existingIndex + $length] -eq $newNormalized[$newIndex + $length]) {
                ++$length
            }
            $oldWordsAfterMatch = $ExistingWords.Count - ($existingIndex + $length)
            if ($length -ge 4 -and $oldWordsAfterMatch -le 12 -and
                ($length -gt $bestLength -or
                 ($length -eq $bestLength -and $newIndex + $length -gt $bestNewEnd))) {
                $bestLength = $length
                $bestNewEnd = $newIndex + $length
            }
        }
    }
    if ($bestLength -ge 4) { return $bestNewEnd }
    return 0
}

function Test-RepetitiveTranscript([string]$Transcript) {
    $words = @(Get-NormalizedWords @($Transcript -split '\s+' | Where-Object { $_ }))
    for ($blockSize = 1; $blockSize -le 12; ++$blockSize) {
        $repeatCount = if ($blockSize -eq 1) { 6 } elseif ($blockSize -eq 2) { 4 } else { 3 }
        for ($start = 0; $start + $repeatCount * $blockSize -le $words.Count; ++$start) {
            $matches = $true
            for ($repeat = 1; $repeat -lt $repeatCount -and $matches; ++$repeat) {
                for ($offset = 0; $offset -lt $blockSize; ++$offset) {
                    if ($words[$start + $offset] -ne
                        $words[$start + $repeat * $blockSize + $offset]) {
                        $matches = $false
                        break
                    }
                }
            }
            if ($matches) { return $true }
        }
    }
    return $false
}

function Read-ProbeResult([string]$StdoutPath, [string]$StderrPath, [int]$Index, [double]$StartSeconds, [double]$DurationSeconds, [double]$WallMilliseconds) {
    $text = [System.IO.File]::ReadAllText($StdoutPath, [System.Text.Encoding]::UTF8)
    $lines = $text -split "`r?`n"
    $marker = [Array]::IndexOf($lines, 'Transcript (German):')
    if ($marker -lt 0) {
        $marker = [Array]::IndexOf($lines, 'Transcript (German, greedy):')
    }
    if ($marker -lt 0) { throw "Segment $Index has no transcript marker. See $StdoutPath" }
    $timingLine = -1
    for ($lineIndex = $marker + 1; $lineIndex -lt $lines.Count; ++$lineIndex) {
        if ($lines[$lineIndex] -match '^  decoder time:') {
            $timingLine = $lineIndex
            break
        }
    }
    if ($timingLine -lt 0) { throw "Segment $Index has no decoder timing. See $StdoutPath" }
    $transcript = (($lines[($marker + 1)..($timingLine - 1)] -join "`n").Trim())
    $timings = @{}
    foreach ($line in $lines) {
        if ($line -match '^  (.+): ([0-9.]+) us$') {
            $timings[$matches[1]] = [double]$matches[2]
        }
    }
    $generatedTokens = 0
    $decoderSteps = 0
    $decoderWorkers = 0
    foreach ($line in $lines) {
        if ($line -match '^  generated tokens: ([0-9]+)$') { $generatedTokens = [int]$matches[1] }
        if ($line -match '^  decoder steps: ([0-9]+)$') { $decoderSteps = [int]$matches[1] }
        if ($line -match '^  decoder workers: ([0-9]+)$') { $decoderWorkers = [int]$matches[1] }
    }
    return [pscustomobject]@{
        index = $Index
        start_seconds = $StartSeconds
        duration_seconds = $DurationSeconds
        timestamp = Format-Timestamp $StartSeconds
        transcript = $transcript
        generated_tokens = $generatedTokens
        decoder_steps = $decoderSteps
        decoder_workers = $decoderWorkers
        truncated = ($generatedTokens -ge 256)
        repetitive = (Test-RepetitiveTranscript $transcript)
        overlap_words_removed = 0
        wall_ms = if ($timings.ContainsKey('window total time')) {
            $timings['window total time'] / 1000.0
        } else { $WallMilliseconds }
        log_mel_ms = $timings['WAV to log-mel time'] / 1000.0
        context_restore_ms = $timings['contextCreateFromBinary time'] / 1000.0
        frontend_conv1_ms = $timings['frontend conv1 time'] / 1000.0
        frontend_conv2_ms = $timings['frontend conv2/position time'] / 1000.0
        encoder_ms = $timings['cached first execution'] / 1000.0
        cross_kv_npu_ms = $timings['decoder cross K/V NPU time'] / 1000.0
        decoder_ms = $timings['decoder time'] / 1000.0
        cross_kv_import_ms = $timings['cross K/V precompute'] / 1000.0
        self_attention_ms = $timings['CPU self-attention'] / 1000.0
        cross_attention_ms = $timings['CPU cross-attention'] / 1000.0
        feed_forward_ms = $timings['CPU feed-forward'] / 1000.0
        logits_ms = $timings['CPU final norm/logits'] / 1000.0
        stdout = [System.IO.Path]::GetFileName($StdoutPath)
        stderr = [System.IO.Path]::GetFileName($StderrPath)
    }
}

if (-not $SkipConversion) {
    Write-Host "Converting source to 16 kHz mono float32 WAV: $FullWav"
    & ffmpeg -hide_banner -loglevel error -y -i $InputMp3 -map '0:a:0' -vn -ac 1 -ar 16000 -c:a pcm_f32le $FullWav
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg conversion failed with exit code $LASTEXITCODE." }
} elseif (-not (Test-Path $FullWav)) {
    throw "SkipConversion was requested, but the WAV does not exist: $FullWav"
}

$probeOutput = & ffprobe -v error -select_streams a:0 -show_entries 'stream=codec_name,sample_rate,channels:format=duration' -of 'default=noprint_wrappers=1' $FullWav
if ($LASTEXITCODE -ne 0) { throw "ffprobe failed with exit code $LASTEXITCODE." }
$metadata = @{}
foreach ($line in $probeOutput) {
    $parts = $line -split '=', 2
    if ($parts.Count -eq 2) { $metadata[$parts[0]] = $parts[1] }
}
if ($metadata['codec_name'] -ne 'pcm_f32le' -or $metadata['sample_rate'] -ne '16000' -or $metadata['channels'] -ne '1') {
    throw 'The inference WAV must be mono 16 kHz pcm_f32le.'
}
$durationSeconds = [double]::Parse($metadata['duration'], [System.Globalization.CultureInfo]::InvariantCulture)
$stepSeconds = $WindowSeconds - $OverlapSeconds
$segmentCount = [int][math]::Ceiling([math]::Max(0.0, $durationSeconds - $OverlapSeconds) / $stepSeconds)
if ($MaximumSegments -gt 0) { $segmentCount = [math]::Min($segmentCount, $MaximumSegments) }
Write-Host ("Audio duration {0:N3} s; processing {1} windows ({2:N1} s window, {3:N1} s overlap)." -f $durationSeconds, $segmentCount, $WindowSeconds, $OverlapSeconds)

$results = New-Object System.Collections.Generic.List[object]
$pending = New-Object System.Collections.Generic.List[object]
$probeProcessCount = 0
$probeProcessWallMs = 0.0
$windowsInferredThisRun = 0
for ($index = 0; $index -lt $segmentCount; ++$index) {
    $startSeconds = $index * $stepSeconds
    $remainingSeconds = $durationSeconds - $startSeconds
    $segmentDuration = [math]::Min($WindowSeconds, $remainingSeconds)
    if ($segmentDuration -le 0.0) { break }
    $baseName = 'segment-{0:D4}' -f $index
    $windowPath = Join-Path $windowDirectory ($baseName + '.wav')
    $stdoutPath = Join-Path $logDirectory ($baseName + '.stdout.txt')
    $stderrPath = Join-Path $logDirectory ($baseName + '.stderr.txt')
    $resultPath = Join-Path $resultDirectory ($baseName + '.json')
    if ($Resume -and (Test-Path $resultPath)) {
        $result = Get-Content -Raw -Encoding UTF8 $resultPath | ConvertFrom-Json
        $isRepetitive = Test-RepetitiveTranscript $result.transcript
        if ($result.PSObject.Properties['repetitive'] -eq $null) {
            $result | Add-Member -NotePropertyName repetitive -NotePropertyValue $isRepetitive
        } else { $result.repetitive = $isRepetitive }
        if (-not $RetryFlagged -or (-not $result.truncated -and -not $result.repetitive)) {
            $results.Add($result)
            Write-Host ("[{0}/{1}] {2} resumed" -f ($index + 1), $segmentCount, (Format-Timestamp $startSeconds))
            continue
        }
        Write-Host ("[{0}/{1}] {2} retrying flagged window" -f ($index + 1), $segmentCount, (Format-Timestamp $startSeconds))
    }

    & ffmpeg -hide_banner -loglevel error -y -ss $startSeconds.ToString('0.######', [System.Globalization.CultureInfo]::InvariantCulture) -i $FullWav -t $segmentDuration.ToString('0.######', [System.Globalization.CultureInfo]::InvariantCulture) -ac 1 -ar 16000 -c:a pcm_f32le $windowPath
    if ($LASTEXITCODE -ne 0) { throw "Window conversion failed for segment $index." }
    if ($PerWindowProcesses) {
        $probeResult = Invoke-Probe $windowPath $stdoutPath $stderrPath
        ++$probeProcessCount
        ++$windowsInferredThisRun
        $probeProcessWallMs += $probeResult.WallMilliseconds
        if ($probeResult.ExitCode -ne 0) {
            throw "Probe failed for segment $index with exit code $($probeResult.ExitCode). See $stderrPath"
        }
        $result = Read-ProbeResult $stdoutPath $stderrPath $index $startSeconds $segmentDuration $probeResult.WallMilliseconds
        Write-Utf8File $resultPath (($result | ConvertTo-Json -Compress) + "`n")
        $results.Add($result)
        if (-not $KeepWindows) { Remove-Item -Force $windowPath }
        Write-Host ("[{0}/{1}] {2} tokens={3} decoder={4:N1} ms wall={5:N1} ms" -f ($index + 1), $segmentCount, $result.timestamp, $result.generated_tokens, $result.decoder_ms, $result.wall_ms)
    } else {
        $pending.Add([pscustomobject]@{
            index = $index
            start_seconds = $startSeconds
            duration_seconds = $segmentDuration
            window_path = $windowPath
            stdout_path = $stdoutPath
            stderr_path = $stderrPath
            result_path = $resultPath
        })
        Write-Host ("[{0}/{1}] {2} prepared" -f ($index + 1), $segmentCount, (Format-Timestamp $startSeconds))
    }
}

if (-not $PerWindowProcesses -and $pending.Count -gt 0) {
    $manifestPath = Join-Path $OutputDirectory 'window-manifest.txt'
    $batchStdoutPath = Join-Path $logDirectory 'batch.stdout.txt'
    $batchStderrPath = Join-Path $logDirectory 'batch.stderr.txt'
    Write-Utf8File $manifestPath ((@($pending | ForEach-Object { $_.window_path }) -join "`r`n") + "`r`n")
    Write-Host ("Running {0} windows in one persistent probe process." -f $pending.Count)
    $probeResult = Invoke-Probe ('@' + $manifestPath) $batchStdoutPath $batchStderrPath
    ++$probeProcessCount
    $windowsInferredThisRun += $pending.Count
    $probeProcessWallMs += $probeResult.WallMilliseconds
    $batchText = [System.IO.File]::ReadAllText($batchStdoutPath, [System.Text.Encoding]::UTF8)
    $blocks = Split-BatchOutput $batchText
    for ($batchIndex = 0; $batchIndex -lt $pending.Count; ++$batchIndex) {
        $item = $pending[$batchIndex]
        if (-not $blocks.ContainsKey($batchIndex)) {
            throw "Persistent probe returned no output for segment $($item.index)."
        }
        Write-Utf8File $item.stdout_path $blocks[$batchIndex]
        $result = Read-ProbeResult $item.stdout_path $batchStderrPath $item.index $item.start_seconds $item.duration_seconds 0.0
        $result.stderr = [System.IO.Path]::GetFileName($batchStderrPath)
        Write-Utf8File $item.result_path (($result | ConvertTo-Json -Compress) + "`n")
        $results.Add($result)
        if (-not $KeepWindows) { Remove-Item -Force $item.window_path }
        Write-Host ("[{0}/{1}] {2} tokens={3} decoder={4:N1} ms window={5:N1} ms" -f ($item.index + 1), $segmentCount, $result.timestamp, $result.generated_tokens, $result.decoder_ms, $result.wall_ms)
    }
    if ($probeResult.ExitCode -ne 0) {
        throw "Persistent probe failed with exit code $($probeResult.ExitCode). See $batchStderrPath"
    }
    Write-Host ("Persistent probe process wall time: {0:N1} ms" -f $probeResult.WallMilliseconds)
}

$results = @($results | Sort-Object index)

$stitchedWords = New-Object System.Collections.Generic.List[string]
$segmentLines = New-Object System.Collections.Generic.List[string]
foreach ($result in $results) {
    $segmentLines.Add(('[{0}] {1}' -f $result.timestamp, $result.transcript))
    $newWords = @($result.transcript -split '\s+' | Where-Object { $_ })
    $existingWords = @($stitchedWords)
    $overlapWordCount = Get-OverlapWordCount $existingWords $newWords
    $result.overlap_words_removed = $overlapWordCount
    for ($wordIndex = $overlapWordCount; $wordIndex -lt $newWords.Count; ++$wordIndex) {
        $stitchedWords.Add($newWords[$wordIndex])
    }
}

Write-Utf8File (Join-Path $OutputDirectory 'transcript-segments.txt') (($segmentLines -join "`r`n`r`n") + "`r`n")
Write-Utf8File (Join-Path $OutputDirectory 'transcript.txt') (($stitchedWords -join ' ') + "`r`n")
Write-Utf8File (Join-Path $OutputDirectory 'timings.csv') ((($results | ConvertTo-Csv -NoTypeInformation) -join "`r`n") + "`r`n")
Write-Utf8File (Join-Path $OutputDirectory 'segments.jsonl') ((@($results | ForEach-Object { $_ | ConvertTo-Json -Compress }) -join "`r`n") + "`r`n")

$stageNames = @('log_mel_ms', 'context_restore_ms', 'frontend_conv1_ms', 'frontend_conv2_ms', 'encoder_ms', 'cross_kv_npu_ms', 'cross_kv_import_ms', 'self_attention_ms', 'cross_attention_ms', 'feed_forward_ms', 'logits_ms')
$summaryLines = New-Object System.Collections.Generic.List[string]
$totalWallMs = ($results | Measure-Object -Property wall_ms -Sum).Sum
$effectiveWallMs = if ($windowsInferredThisRun -eq $results.Count -and $probeProcessWallMs -gt 0.0) {
    $probeProcessWallMs
} else { $totalWallMs }
$totalAudioSeconds = if ($MaximumSegments -gt 0) { ($results | Measure-Object -Property duration_seconds -Sum).Sum } else { $durationSeconds }
$summaryLines.Add(('audio_seconds={0:F3}' -f $totalAudioSeconds))
$summaryLines.Add(('windows={0}' -f $results.Count))
$summaryLines.Add(('windows_inferred_this_run={0}' -f $windowsInferredThisRun))
$summaryLines.Add(('probe_processes_this_run={0}' -f $probeProcessCount))
$summaryLines.Add(('probe_process_wall_seconds_this_run={0:F3}' -f ($probeProcessWallMs / 1000.0)))
$summaryLines.Add(('window_wall_seconds={0:F3}' -f ($totalWallMs / 1000.0)))
$summaryLines.Add(('wall_seconds={0:F3}' -f ($effectiveWallMs / 1000.0)))
$summaryLines.Add(('realtime_factor={0:F6}' -f (($effectiveWallMs / 1000.0) / $totalAudioSeconds)))
$summaryLines.Add(('generated_tokens={0}' -f (($results | Measure-Object -Property generated_tokens -Sum).Sum)))
$summaryLines.Add(('truncated_windows={0}' -f (@($results | Where-Object { $_.truncated }).Count)))
$summaryLines.Add(('repetitive_windows={0}' -f (@($results | Where-Object { $_.repetitive }).Count)))
foreach ($stageName in $stageNames) {
    $sum = ($results | Measure-Object -Property $stageName -Sum).Sum
    $summaryLines.Add(('{0}_total={1:F3}' -f $stageName, $sum))
    $summaryLines.Add(('{0}_mean={1:F3}' -f $stageName, ($sum / $results.Count)))
}
Write-Utf8File (Join-Path $OutputDirectory 'timings-summary.txt') (($summaryLines -join "`r`n") + "`r`n")
Write-Host "Transcript: $(Join-Path $OutputDirectory 'transcript.txt')"
Write-Host "Segment transcript: $(Join-Path $OutputDirectory 'transcript-segments.txt')"
Write-Host "Timing summary: $(Join-Path $OutputDirectory 'timings-summary.txt')"