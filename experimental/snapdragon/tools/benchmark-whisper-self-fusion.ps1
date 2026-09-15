param(
    [string]$OutputDirectory = 'experimental/snapdragon/data/self-fusion-medium-20260915',
    [ValidateRange(1, 10)][int]$Repetitions = 3,
    [switch]$ValidateOnly,
    [switch]$ReportOnly
)

$ErrorActionPreference = 'Stop'
function Write-Comparison([string]$Directory, [int]$PairCount) {
    $runs = ConvertFrom-Json -InputObject ([IO.File]::ReadAllText((Join-Path $Directory 'runs.json')))
    if ($runs.Count -ne 2 + 2 * $PairCount) { throw 'The accepted run collection is incomplete' }
    $long = @($runs | Where-Object clip -eq long)
    if ($long.Count -ne 2 * $PairCount) { throw 'Unexpected long-run count' }
    function Get-Median($Values) {
        $sorted = @($Values | Sort-Object)
        $middle = [int][Math]::Floor($sorted.Count / 2)
        if ($sorted.Count % 2) { return $sorted[$middle] }
        return ($sorted[$middle - 1] + $sorted[$middle]) / 2
    }
    $comparison = [ordered]@{ accepted_runs = $runs.Count; long_runs = $long.Count; pairs = @(); medians = @{} }
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.Add('# Full-Medium Self-Fusion Benchmark')
    $lines.Add('')
    $lines.Add('Windows ARM64, Snapdragon X Elite, QAIRT 2.50.0.260828, diagnostics off. A 35.008-second correctness pair precedes alternating 300-second pairs. Builds and context generation are excluded; process wall time includes loading, restore, transcription and cleanup.')
    $lines.Add('')
    $lines.Add('| Pair | Split Wall s | Fused Wall s | Wall Reduction % | Split CPU s | Fused CPU s | CPU Reduction % |')
    $lines.Add('| --- | ---: | ---: | ---: | ---: | ---: | ---: |')
    for ($pair = 1; $pair -le $PairCount; ++$pair) {
        $split = @($long | Where-Object { $_.repetition -eq $pair -and $_.variant -eq 'split' })
        $fused = @($long | Where-Object { $_.repetition -eq $pair -and $_.variant -eq 'fused' })
        if ($split.Count -ne 1 -or $fused.Count -ne 1) { throw "Missing or duplicate pair $pair" }
        $wallReduction = 100 * (1 - $fused[0].wall_seconds / $split[0].wall_seconds)
        $cpuReduction = 100 * (1 - $fused[0].process_cpu_seconds / $split[0].process_cpu_seconds)
        $comparison.pairs += [ordered]@{ repetition = $pair; split_wall = $split[0].wall_seconds; fused_wall = $fused[0].wall_seconds;
            wall_reduction_percent = $wallReduction; split_cpu = $split[0].process_cpu_seconds; fused_cpu = $fused[0].process_cpu_seconds; cpu_reduction_percent = $cpuReduction }
        $lines.Add([string]::Format([Globalization.CultureInfo]::InvariantCulture,
            '| {0} | {1:F3} | {2:F3} | {3:F2} | {4:F3} | {5:F3} | {6:F2} |',
            $pair, $split[0].wall_seconds, $fused[0].wall_seconds, $wallReduction,
            $split[0].process_cpu_seconds, $fused[0].process_cpu_seconds, $cpuReduction))
    }
    $lines.Add('')
    $lines.Add('| Metric (Median) | Split | Fused | Reduction % |')
    $lines.Add('| --- | ---: | ---: | ---: |')
    foreach ($field in @('wall_seconds', 'process_cpu_seconds', 'peak_resident_bytes', 'peak_private_committed_bytes',
        'context_restore_ms', 'npu_self_attention_execute_ms', 'npu_host_call_ms', 'native_cleanup_ms')) {
        $splitValue = Get-Median @($long | Where-Object variant -eq split | ForEach-Object { $_.$field })
        $fusedValue = Get-Median @($long | Where-Object variant -eq fused | ForEach-Object { $_.$field })
        $reduction = 100 * (1 - $fusedValue / $splitValue)
        $comparison.medians[$field] = [ordered]@{ split = $splitValue; fused = $fusedValue; reduction_percent = $reduction }
        $lines.Add([string]::Format([Globalization.CultureInfo]::InvariantCulture, '| {0} | {1:F3} | {2:F3} | {3:F2} |', $field, $splitValue, $fusedValue, $reduction))
    }
    $comparison.transcript_sha256 = $long[0].transcript_sha256
    $comparison.work_counts = @($long | Select-Object variant, repetition, windows, generated_tokens, decoder_steps, prefix_reused_steps,
        npu_self_attention_graph_submissions, npu_fused_cross_mlp_graph_submissions, npu_final_projection_graph_submissions)
    $lines.Add('')
    $lines.Add('All accepted pairs match transcript and work counts. Self submissions are halved; CPU self fallback is absent. Exact executable/runtime/context hashes are in inventory.json; raw per-run summaries and output are preserved beside this report.')
    $lines.Add('')
    $lines.Add('The fused ping-pong cache adds 42 MiB of application shared cache across 24 layers. Peak process memory also includes runtime allocation and placement effects. These sequential, warmed samples are not confidence intervals or cold-start measurements. Host graphExecute duration is not accelerator occupancy. No DDR, device-clock or thermal measurements were collected. The candidate remains opt-in; normal deployment and original context are unchanged.')
    $utf8 = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText((Join-Path $Directory 'comparison.json'), ($comparison | ConvertTo-Json -Depth 8), $utf8)
    [IO.File]::WriteAllLines((Join-Path $Directory 'report.md'), $lines, $utf8)
    Write-Output ($comparison.medians | ConvertTo-Json -Depth 4)
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
Push-Location $repoRoot
try {
    if ($ReportOnly) { Write-Comparison ([IO.Path]::GetFullPath($OutputDirectory)) $Repetitions; return }
    $variants = @{
        split = 'experimental/snapdragon/build/self-fusion-baseline'
        fused = 'experimental/snapdragon/build/self-fusion-candidate'
    }
    $fixtures = @{
        short = 'experimental/snapdragon/data/long-form-35s.wav'
        long = 'experimental/snapdragon/data/bundestag-hearing-5min-16k-mono-f32.wav'
    }
    foreach ($clip in @('short', 'long')) {
        $durationText = & ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 $fixtures[$clip]
        if ($LASTEXITCODE -ne 0) { throw 'Could not inspect benchmark audio' }
        $duration = [double]::Parse($durationText.Trim(), [Globalization.CultureInfo]::InvariantCulture)
        $expected = if ($clip -eq 'short') { 35.008 } else { 300.0 }
        if ([Math]::Abs($duration - $expected) -gt 0.01) { throw "Wrong $clip audio duration: $duration" }
    }
    $cachePaths = @(
        'experimental/snapdragon/build/whisper-medium-m1c-encoder-fp16-l24.qnnctx',
        'experimental/snapdragon/build/whisper-medium-m1c-self-fused-v1-encoder-fp16-l24.qnnctx'
    )
    $cacheHashes = @(Get-FileHash -LiteralPath $cachePaths -Algorithm SHA256 | Select-Object Path, Hash)
    if ($cacheHashes[0].Hash -ne 'FB998248AE4060E646F9E006DD9FDBE25E74727A7FE4C8438A0D4FA52ADBBDA9') { throw 'Original split context changed' }
    $inventory = [ordered]@{ caches = $cacheHashes; power_scheme = (powercfg /getactivescheme | Out-String).Trim(); variants = @{} }
    $runtimeHashes = @{}
    foreach ($variant in @('split', 'fused')) {
        $directory = $variants[$variant]
        $inventory.variants[$variant] = @(Get-ChildItem $directory -File |
            Where-Object { $_.Extension -in @('.dll', '.so', '.cat') -or $_.Name -eq 'npu_probe.exe' } |
            Get-FileHash -Algorithm SHA256 | Select-Object Path, Hash)
        foreach ($file in (Get-ChildItem $directory -File | Where-Object { $_.Extension -in @('.dll', '.so', '.cat') })) {
            $hash = (Get-FileHash $file.FullName -Algorithm SHA256).Hash
            if ($variant -eq 'split') { $runtimeHashes[$file.Name] = $hash }
            elseif (-not $runtimeHashes.ContainsKey($file.Name) -or $runtimeHashes[$file.Name] -ne $hash) { throw "Runtime mismatch: $($file.Name)" }
        }
        $executable = Join-Path $directory 'npu_probe.exe'
        $imports = & llvm-readobj --coff-imports $executable | Out-String
        if ($LASTEXITCODE -ne 0 -or $imports -notmatch 'Name: KERNEL32.dll' -or
            ([regex]::Matches($imports, 'Name: .*\.dll', 'IgnoreCase')).Count -ne 1) { throw "Unexpected imports: $executable" }
    }
    if ($runtimeHashes.Count -eq 0) { throw 'No staged runtime' }
    Write-Output 'PASS audio durations, original cache hash, runtime hashes and no-CRT imports'
    if ($ValidateOnly) { return }
    if (Test-Path $OutputDirectory) { throw 'Output directory already exists; choose a new directory' }
    if (Get-Process npu_probe,npu_probe_builder,self-fusion-probe -ErrorAction SilentlyContinue) { throw 'Another inference or builder is running' }
    New-Item -ItemType Directory $OutputDirectory | Out-Null
    $utf8 = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'inventory.json'), ($inventory | ConvertTo-Json -Depth 8), $utf8)
    $runs = New-Object 'System.Collections.Generic.List[object]'
    $references = @{}
    $cases = @(@{ clip = 'short'; repetition = 0; order = @('split', 'fused') })
    for ($repetition = 1; $repetition -le $Repetitions; ++$repetition) {
        $order = if ($repetition % 2) { @('split', 'fused') } else { @('fused', 'split') }
        $cases += @{ clip = 'long'; repetition = $repetition; order = $order }
    }
    foreach ($case in $cases) {
        foreach ($variant in $case.order) {
            $output = Join-Path $OutputDirectory ('{0}-{1}-{2}' -f $case.clip, $case.repetition, $variant)
            Write-Output ('RUN {0}' -f $output)
            & (Join-Path $PSScriptRoot 'profile-whisper.ps1') -Model medium -DecoderOffload 'fused,self,logits' -Diagnostics off `
                -ProbePath (Join-Path $variants[$variant] 'npu_probe.exe') -WavPath $fixtures[$case.clip] -OutputDirectory $output | Out-Null
            $summary = Get-Content (Join-Path $output 'medium-summary.json') -Raw | ConvertFrom-Json
            if ($summary.exit_code -ne 0 -or $summary.generated_tokens -le 0 -or $summary.context_restore_ms -le 0) { throw "Inference/restore failed: $output" }
            $messages = (Get-Content (Join-Path $output 'medium.stderr.txt') -Raw) + (Get-Content (Join-Path $output 'medium.stdout.txt') -Raw)
            if ($messages -match '<E>|DMA error|CPU fallback' -or $summary.self_attention_ms -gt 0) { throw "Graph failure/fallback: $output" }
            $calls = ($summary.decoder_steps - $summary.prefix_reused_steps) * 24
            $submissions = if ($variant -eq 'split') { $calls * 2 } else { $calls }
            if ($summary.npu_self_attention_offload_calls -ne $calls -or $summary.npu_self_attention_graph_submissions -ne $submissions) { throw "Wrong self graph count: $output" }
            if ($case.clip -eq 'long' -and $summary.transcript_sha256 -ne 'a609b84717a2e1699d94a53a353a25e1b3dd2151425552095d81208ee0632dd0') { throw "Changed established five-minute transcript: $output" }
            if ($references.ContainsKey($case.clip)) {
                foreach ($field in @('transcript_sha256', 'wav_sha256', 'generated_tokens', 'decoder_steps', 'prefix_reused_steps', 'windows',
                    'npu_self_attention_offload_calls', 'npu_fused_cross_mlp_graph_submissions', 'npu_final_projection_graph_submissions')) {
                    if ($summary.$field -ne $references[$case.clip].$field) { throw "Changed $field in $output" }
                }
            } else { $references[$case.clip] = $summary }
            $expectedExeHash = ($inventory.variants[$variant] | Where-Object { [IO.Path]::GetFileName($_.Path) -eq 'npu_probe.exe' }).Hash
            if ($summary.probe_sha256 -ne $expectedExeHash) { throw 'Executable changed during benchmark' }
            $summary | Add-Member clip $case.clip
            $summary | Add-Member variant $variant
            $summary | Add-Member repetition $case.repetition
            $runs.Add($summary)
            [IO.File]::WriteAllText((Join-Path $OutputDirectory 'runs.json'), ($runs.ToArray() | ConvertTo-Json -Depth 8), $utf8)
            Write-Output ('PASS {0}: wall={1:F3}s CPU={2:F3}s self submissions={3}' -f $variant, $summary.wall_seconds, $summary.process_cpu_seconds, $submissions)
        }
    }
    foreach ($cache in $cacheHashes) {
        if ((Get-FileHash $cache.Path -Algorithm SHA256).Hash -ne $cache.Hash) { throw 'Context changed during measurement' }
    }
    Write-Output ('PASS {0} accepted runs including the short correctness gate' -f $runs.Count)
    Write-Comparison ([IO.Path]::GetFullPath($OutputDirectory)) $Repetitions
} finally {
    Pop-Location
}