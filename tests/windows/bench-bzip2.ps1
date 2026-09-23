param(
    [string]$BuildDir = 'build/normal',
    [string]$Workers = '2,4,8,12',
    [ValidateRange(1, 20)][int]$Repetitions = 3,
    [ValidateRange(2, 64)][int]$InputMiB = 12,
    [ValidateSet('Random', 'Mixed')][string]$Pattern = 'Random',
    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$executable = (Resolve-Path (Join-Path $repoRoot (Join-Path $BuildDir 'bunzip2.exe'))).Path
$scratch = Join-Path $repoRoot ('tests/tmp/windows-bzip2-' + [guid]::NewGuid().ToString('N'))
$widths = @($Workers.Split(',') | ForEach-Object {
    $width = 0
    if (-not [int]::TryParse($_, [ref]$width) -or $width -lt 2 -or $width -gt 32) {
        throw 'Worker counts must be comma-separated integers between 2 and 32'
    }
    $width
} | Select-Object -Unique)

function Invoke-Decode([int]$Width) {
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $executable
    $startInfo.Arguments = '"' + $archive + '"'
    $startInfo.WorkingDirectory = $repoRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.EnvironmentVariables['NEWOS_BUNZIP2_WORKERS'] = [string]$Width
    $startInfo.EnvironmentVariables['NEWOS_BUNZIP2_TIMINGS'] = '1'
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $process = [Diagnostics.Process]::Start($startInfo)
    try {
        $diagnostics = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        $clock.Stop()
        if ($process.ExitCode -ne 0) { throw "bunzip2 failed with $Width workers: $diagnostics" }
    } finally {
        $process.Dispose()
    }
    $match = [regex]::Match($diagnostics, 'scan_ns=(\d+) decode_ns=(\d+) write_ns=(\d+) blocks=(\d+)')
    if (-not $match.Success -or [int]$match.Groups[4].Value -lt 2) { throw "Missing multi-block phase timings: $diagnostics" }
    $output = Join-Path $scratch 'decoded'
    if ((Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash -ne $inputHash) { throw "Decoded content mismatch with $Width workers" }
    [pscustomobject]@{
        Workers = $Width
        WallMs = $clock.Elapsed.TotalMilliseconds
        ScanMs = [double]::Parse($match.Groups[1].Value) / 1000000.0
        DecodeMs = [double]::Parse($match.Groups[2].Value) / 1000000.0
        WriteMs = [double]::Parse($match.Groups[3].Value) / 1000000.0
        Blocks = [int]$match.Groups[4].Value
    }
}

[void][IO.Directory]::CreateDirectory($scratch)
$completed = $false
try {
    $inputPath = Join-Path $scratch 'input.bin'
    $archive = Join-Path $scratch 'decoded.bz2'
    $random = [Random]::new(4107)
    $data = [byte[]]::new($InputMiB * 1024 * 1024)
    $random.NextBytes($data)
    if ($Pattern -eq 'Mixed') {
        for ($offset = 0; $offset -lt $data.Length; $offset += 256) {
            [Array]::Clear($data, $offset, 128)
        }
    }
    [IO.File]::WriteAllBytes($inputPath, $data)
    $inputHash = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash
    @'
import bz2
import sys

with open(sys.argv[1], 'rb') as source:
    data = source.read()
with open(sys.argv[2], 'wb') as target:
    target.write(bz2.compress(data, compresslevel=9))
'@ | & $Python -B - $inputPath $archive
    if ($LASTEXITCODE -ne 0) { throw 'Python could not generate a standard bzip2 fixture' }
    Write-Output ('input_mib={0} compressed_bytes={1}' -f $InputMiB, (Get-Item $archive).Length)
    foreach ($width in $widths) { $null = Invoke-Decode $width }
    $samples = @()
    for ($trial = 0; $trial -lt $Repetitions; ++$trial) {
        $order = if ($trial % 2 -eq 0) { $widths } else { @($widths | Sort-Object -Descending) }
        foreach ($width in $order) { $samples += Invoke-Decode $width }
    }
    Write-Output 'workers blocks wall_ms scan_ms decode_ms write_ms scan_pct'
    foreach ($width in $widths) {
        $group = @($samples | Where-Object { $_.Workers -eq $width })
        $middle = [int][math]::Floor($group.Count / 2)
        $wall = @($group | Sort-Object WallMs)[$middle].WallMs
        $scan = @($group | Sort-Object ScanMs)[$middle].ScanMs
        $decode = @($group | Sort-Object DecodeMs)[$middle].DecodeMs
        $write = @($group | Sort-Object WriteMs)[$middle].WriteMs
        '{0,7} {1,6} {2,7:F1} {3,7:F1} {4,9:F1} {5,8:F1} {6,7:P0}' -f $width, $group[0].Blocks, $wall, $scan, $decode, $write, ($scan / $wall)
    }
    $completed = $true
} finally {
    if ($completed) { [IO.Directory]::Delete($scratch, $true) }
    else { Write-Warning "Fixture retained for diagnosis: $scratch" }
}