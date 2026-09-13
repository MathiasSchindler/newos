param(
    [string]$WavPath,
    [ValidateSet('tiny', 'base', 'small')]
    [string]$Model = 'small',
    [string[]]$Modes = @('cross,mlp', 'fused', 'fused,self,logits'),
    [ValidateRange(1, 20)]
    [int]$Repetitions = 3,
    [int]$DecoderWorkers = 0,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
if (-not $WavPath) {
    $WavPath = Join-Path $repoRoot 'experimental\snapdragon\data\bundestag-hearing-5min-16k-mono-f32.wav'
}
if (-not $OutputDirectory) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path $repoRoot "experimental\snapdragon\data\offload-benchmark-$stamp"
}
$WavPath = [System.IO.Path]::GetFullPath($WavPath)
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$profileScript = Join-Path $PSScriptRoot 'profile-whisper.ps1'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$validModes = @(
    'cpu', 'all', 'cross', 'mlp', 'self', 'logits',
    'cross,mlp', 'cross,mlp,logits', 'cross,mlp,self',
    'fused', 'fused,logits', 'fused,self', 'fused,self,logits'
)
foreach ($mode in $Modes) {
    if ($mode -notin $validModes) { throw "Unsupported decoder offload mode: $mode" }
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

function Get-Percentile([double[]]$Values, [double]$Fraction) {
    if ($Values.Count -eq 0) { return 0.0 }
    $ordered = @($Values | Sort-Object)
    $index = [math]::Ceiling($Fraction * $ordered.Count) - 1
    if ($index -lt 0) { $index = 0 }
    return [double]$ordered[$index]
}

$runs = [System.Collections.Generic.List[object]]::new()
for ($repetition = 1; $repetition -le $Repetitions; ++$repetition) {
    $orderedModes = if (($repetition % 2) -eq 0) {
        @($Modes[($Modes.Count - 1)..0])
    } else {
        @($Modes)
    }
    for ($order = 0; $order -lt $orderedModes.Count; ++$order) {
        $mode = $orderedModes[$order]
        $safeMode = $mode.Replace(',', '-')
        $runDirectory = Join-Path $OutputDirectory (
            'run-{0:D2}-{1:D2}-{2}' -f $repetition, ($order + 1), $safeMode
        )
        $arguments = @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $profileScript,
            '-Model', $Model, '-DecoderOffload', $mode,
            '-WavPath', $WavPath, '-OutputDirectory', $runDirectory
        )
        if ($DecoderWorkers -gt 0) { $arguments += @('-DecoderWorkers', $DecoderWorkers) }
        & powershell.exe @arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Profile failed for repetition $repetition mode $mode."
        }
        $summaryPath = Join-Path $runDirectory "$Model-summary.json"
        $summary = Get-Content -Raw -LiteralPath $summaryPath | ConvertFrom-Json
        $summary | Add-Member -NotePropertyName repetition -NotePropertyValue $repetition
        $summary | Add-Member -NotePropertyName run_order -NotePropertyValue ($order + 1)
        $runs.Add($summary)
    }
}

$aggregate = foreach ($mode in $Modes) {
    $items = @($runs | Where-Object decoder_offload -eq $mode)
    $wall = [double[]]@($items.wall_seconds)
    $cpu = [double[]]@($items.process_cpu_seconds)
    $native = [double[]]@($items.native_process_ms)
    $duty = [double[]]@($items.npu_host_call_duty_percent)
    $hashes = @($items.transcript_sha256 | Sort-Object -Unique)
    [pscustomobject]@{
        model = $Model
        decoder_offload = $mode
        repetitions = $items.Count
        wall_median_s = Get-Percentile $wall 0.5
        wall_p95_s = Get-Percentile $wall 0.95
        cpu_median_s = Get-Percentile $cpu 0.5
        native_process_median_ms = Get-Percentile $native 0.5
        npu_duty_median_percent = Get-Percentile $duty 0.5
        maximum_graph_call_ms = [double](($items | ForEach-Object {
            [math]::Max(
                [math]::Max(
                    [double]$_.npu_self_attention_maximum_ms,
                    [double]$_.npu_cross_attention_maximum_ms
                ),
                [math]::Max(
                    [double]$_.npu_fused_cross_mlp_maximum_ms,
                    [math]::Max(
                        [double]$_.npu_mlp_maximum_ms,
                        [double]$_.npu_final_projection_maximum_ms
                    )
                )
            )
        } | Measure-Object -Maximum).Maximum)
        peak_resident_bytes = [long](($items.peak_resident_bytes | Measure-Object -Maximum).Maximum)
        transcript_hashes = $hashes -join ','
        transcript_stable = $hashes.Count -eq 1
    }
}

$runs | Export-Csv -NoTypeInformation -Encoding utf8 `
    (Join-Path $OutputDirectory 'runs.csv')
$aggregate | Export-Csv -NoTypeInformation -Encoding utf8 `
    (Join-Path $OutputDirectory 'aggregate.csv')
[System.IO.File]::WriteAllText(
    (Join-Path $OutputDirectory 'aggregate.json'),
    ($aggregate | ConvertTo-Json -Depth 4) + "`r`n", $utf8NoBom
)
$aggregate | Format-Table -AutoSize