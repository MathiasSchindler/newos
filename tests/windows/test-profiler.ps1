param(
    [string]$BuildDir = 'build/profile-windows/normal',
    [string]$Profiler = 'build/profile-windows/normal/profiler.exe'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Push-Location $repoRoot
try {
    $trace = Join-Path $repoRoot 'tests/tmp/windows-profile-cat.nprof'
    $env:NEWOS_PROFILE = $trace
    try {
        & (Join-Path $BuildDir 'cat.exe') LICENSE > $null
        if ($LASTEXITCODE -ne 0) { throw 'Profiled cat failed' }
    } finally {
        Remove-Item Env:NEWOS_PROFILE -ErrorAction SilentlyContinue
    }
    $lines = [IO.File]::ReadAllLines($trace)
    if ($lines.Count -lt 10 -or $lines[0] -notmatch '^enter [1-9][0-9]* [1-9][0-9]* 0x[0-9a-f]+$') {
        throw 'Windows runtime did not generate timestamped, thread-aware enter/exit records'
    }
    $catReport = Join-Path $repoRoot 'tests/tmp/windows-profile-cat-report.txt'
    $catDiagnostics = Join-Path $repoRoot 'tests/tmp/windows-profile-cat-diagnostics.txt'
    $ErrorActionPreference = 'Continue'
    try {
        & $Profiler --thread-summary $trace > $catReport 2> $catDiagnostics
        $status = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = 'Stop'
    }
    if ($status -ne 0 -or [IO.File]::ReadAllText($catDiagnostics) -notmatch '(?m)^profiler: thread [0-9]+ [0-9]+ [0-9.]+ [0-9]+ 0 0\s*$') {
        throw 'Windows single-thread profile has unmatched events'
    }
    $nm = Join-Path (Split-Path (Get-Command clang -ErrorAction Stop).Source -Parent) 'llvm-nm.exe'
    $symbols = Join-Path $repoRoot 'tests/tmp/windows-profile-cat.nm'
    & $nm -n (Join-Path $BuildDir 'cat.exe') | Set-Content -Encoding ascii $symbols
    if ($LASTEXITCODE -ne 0) { throw 'Could not export unstripped PE symbols' }
    $ErrorActionPreference = 'Continue'
    try {
        & $Profiler --csv -m $symbols $trace > $catReport 2> $catDiagnostics
        $status = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = 'Stop'
    }
    if ($status -ne 0 -or [IO.File]::ReadAllText($catReport) -notmatch '(?m),main\s*$') {
        throw 'Windows PE symbols did not resolve through image relocation'
    }
    Write-Output ("PASS Windows cat profile: {0} balanced records" -f $lines.Count)

    $input = Join-Path $repoRoot 'tests/tmp/windows-profile-parallel-input.txt'
    $output = Join-Path $repoRoot 'tests/tmp/windows-profile-parallel-output.txt'
    $parallelTrace = Join-Path $repoRoot 'tests/tmp/windows-profile-parallel.nprof'
    $parallelReport = Join-Path $repoRoot 'tests/tmp/windows-profile-parallel-report.txt'
    $parallelDiagnostics = Join-Path $repoRoot 'tests/tmp/windows-profile-parallel-diagnostics.txt'
    $inputLines = [string[]]::new(65536)
    for ($index = 0; $index -lt $inputLines.Length; $index++) {
        $inputLines[$index] = '{0:D8} workload-{1:D5}' -f (65535 - $index), ($index % 10007)
    }
    [IO.File]::WriteAllLines($input, $inputLines, [Text.UTF8Encoding]::new($false))
    $env:NEWOS_SORT_WORKERS = '8'
    $env:NEWOS_PROFILE = $parallelTrace
    $env:NEWOS_PROFILE_WORKER_ONLY = '1'
    $env:NEWOS_PROFILE_MAX_EVENTS = '250000'
    try {
        & (Join-Path $BuildDir 'sort.exe') $input > $output
        if ($LASTEXITCODE -ne 0) { throw 'Profiled parallel sort failed' }
    } finally {
        Remove-Item Env:NEWOS_SORT_WORKERS,Env:NEWOS_PROFILE,Env:NEWOS_PROFILE_WORKER_ONLY,Env:NEWOS_PROFILE_MAX_EVENTS -ErrorAction SilentlyContinue
    }
    if (@([IO.File]::ReadAllLines($output)).Count -ne $inputLines.Length) { throw 'Parallel sort lost input lines' }
    $ErrorActionPreference = 'Continue'
    try {
        & $Profiler --thread-summary -n 3 $parallelTrace > $parallelReport 2> $parallelDiagnostics
        $status = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = 'Stop'
    }
    $diagnostics = [IO.File]::ReadAllText($parallelDiagnostics)
    $threadCount = [regex]::Match($diagnostics, 'threads=([0-9]+)')
    if ($status -ne 0 -or -not $threadCount.Success -or [int]$threadCount.Groups[1].Value -lt 2 -or
        $diagnostics -notmatch 'malformed=0' -or $diagnostics -notmatch 'unmatched_exits=0') {
        throw "Windows parallel profile is invalid: $diagnostics"
    }
    Write-Output ("PASS Windows sort profile: {0} worker threads recorded (bounded trace)" -f $threadCount.Groups[1].Value)
} finally {
    Pop-Location
}