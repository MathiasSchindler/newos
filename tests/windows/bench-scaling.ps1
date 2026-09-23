param(
    [ValidateSet('sort', 'zip')][string]$Tool = 'zip',
    [string]$BuildDir = 'build/normal',
    [string]$Workers = '1,2,4,8,12',
    [ValidateRange(1, 20)][int]$Repetitions = 3,
    [ValidateRange(1, 32)][int]$ZipFiles = 12,
    [switch]$ZipStoreOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$executable = (Resolve-Path (Join-Path $repoRoot (Join-Path $BuildDir "$Tool.exe"))).Path
$scratch = Join-Path $repoRoot ('tests/tmp/windows-scaling-' + [guid]::NewGuid().ToString('N'))
$relativeScratch = 'tests/tmp/' + (Split-Path $scratch -Leaf)
$widths = @($Workers.Split(',') | ForEach-Object {
    $width = 0
    if (-not [int]::TryParse($_, [ref]$width) -or $width -lt 1 -or $width -gt 32) {
        throw 'Worker counts must be comma-separated integers between 1 and 32'
    }
    $width
} | Select-Object -Unique)
if ($widths -notcontains 1) { throw 'Worker counts must include 1 as a speedup baseline' }
if ($ZipStoreOnly -and $Tool -ne 'zip') { throw '-ZipStoreOnly requires -Tool zip' }

function Invoke-Run([int]$Width) {
    $output = Join-Path $relativeScratch "result-$Width"
    $arguments = if ($Tool -eq 'sort') {
        "-o $output.txt $relativeScratch/input.txt"
    } else {
        "$(if ($ZipStoreOnly) { '-0 ' })$output.zip $($script:zipInputs -join ' ')"
    }
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $executable
    $startInfo.Arguments = $arguments
    $startInfo.WorkingDirectory = $repoRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.EnvironmentVariables["NEWOS_$($Tool.ToUpperInvariant())_WORKERS"] = [string]$Width
    $startInfo.EnvironmentVariables['NEWOS_PROFILE'] = '0'
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $process = [Diagnostics.Process]::Start($startInfo)
    try {
        $process.WaitForExit()
        $clock.Stop()
        if ($process.ExitCode -ne 0) { throw "$Tool failed with $Width workers: exit $($process.ExitCode)" }
        $cpuMs = $process.TotalProcessorTime.TotalMilliseconds
    } finally {
        $process.Dispose()
    }
    $outputPath = Join-Path $repoRoot ($output + $(if ($Tool -eq 'sort') { '.txt' } else { '.zip' }))
    $hash = (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash
    if ($script:referenceHash -eq $null) { $script:referenceHash = $hash }
    if ($hash -ne $script:referenceHash) { throw "Output mismatch with $Width workers" }
    [pscustomobject]@{ Workers = $Width; WallMs = $clock.Elapsed.TotalMilliseconds; CpuMs = $cpuMs }
}

[void][IO.Directory]::CreateDirectory($scratch)
$completed = $false
try {
    if ($Tool -eq 'sort') {
        $writer = [IO.StreamWriter]::new((Join-Path $scratch 'input.txt'), $false, [Text.UTF8Encoding]::new($false))
        try {
            for ($index = 131071; $index -ge 0; --$index) {
                $writer.WriteLine(('{0:D8} workload-{1:D5}' -f $index, ($index % 10007)))
            }
        } finally {
            $writer.Dispose()
        }
    } else {
        $script:zipInputs = @()
        $random = [Random]::new(4107)
        for ($index = 0; $index -lt $ZipFiles; ++$index) {
            $name = 'input-{0:D2}.bin' -f $index
            $data = [byte[]]::new(2 * 1024 * 1024)
            $random.NextBytes($data)
            [IO.File]::WriteAllBytes((Join-Path $scratch $name), $data)
            $script:zipInputs += "$relativeScratch/$name"
        }
    }

    $script:referenceHash = $null
    foreach ($width in $widths) { $null = Invoke-Run $width }
    $samples = @()
    for ($trial = 0; $trial -lt $Repetitions; ++$trial) {
        $order = if ($trial % 2 -eq 0) { $widths } else { @($widths | Sort-Object -Descending) }
        foreach ($width in $order) { $samples += Invoke-Run $width }
    }
    $baselineGroup = @($samples | Where-Object { $_.Workers -eq 1 } | Sort-Object WallMs)
    $baseline = $baselineGroup[[int][math]::Floor($baselineGroup.Count / 2)].WallMs
    Write-Output 'workers  wall_ms  cpu_ms  avg_cores  speedup  efficiency'
    foreach ($width in $widths) {
        $group = @($samples | Where-Object { $_.Workers -eq $width })
        $wall = @($group | Sort-Object WallMs)[[int][math]::Floor($group.Count / 2)].WallMs
        $cpu = @($group | Sort-Object CpuMs)[[int][math]::Floor($group.Count / 2)].CpuMs
        $speedup = $baseline / $wall
        '{0,7} {1,8:F1} {2,7:F1} {3,10:F2} {4,8:F2} {5,11:P0}' -f $width, $wall, $cpu, ($cpu / $wall), $speedup, ($speedup / $width)
    }
    $completed = $true
} finally {
    if ($completed) { [IO.Directory]::Delete($scratch, $true) }
    else { Write-Warning "Benchmark fixture retained for diagnosis: $scratch" }
}