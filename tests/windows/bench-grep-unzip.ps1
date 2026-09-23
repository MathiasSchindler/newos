param(
    [string]$BuildDir = 'build/normal',
    [ValidateRange(1, 20)][int]$Repetitions = 3,
    [ValidateRange(2, 64)][int]$Files = 16,
    [string]$Workers = '1,2,4,8',
    [string]$Python = 'python',
    [string]$ProfileDir = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$grep = (Resolve-Path (Join-Path $repoRoot (Join-Path $BuildDir 'grep.exe'))).Path
$unzip = (Resolve-Path (Join-Path $repoRoot (Join-Path $BuildDir 'unzip.exe'))).Path
$scratch = Join-Path $repoRoot ('tests/tmp/windows-grep-unzip-' + [guid]::NewGuid().ToString('N'))

function Invoke-Case([string]$Executable, [string]$Arguments, [string]$WorkerVariable) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.Arguments = $Arguments
    $start.WorkingDirectory = $repoRoot
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables[$WorkerVariable] = '4'
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEnd()
        $errorText = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        return [pscustomobject]@{ ExitCode = $process.ExitCode; Output = $output; Error = $errorText }
    } finally {
        $process.Dispose()
    }
}

[void][IO.Directory]::CreateDirectory($scratch)
$completed = $false
try {
    $source = Join-Path $scratch 'source'
    [void][IO.Directory]::CreateDirectory($source)
    $archive = Join-Path $scratch 'fixture.zip'
    @'
import pathlib
import random
import sys
import zipfile

root = pathlib.Path(sys.argv[1])
count = int(sys.argv[3])
rng = random.Random(4107)
with zipfile.ZipFile(sys.argv[2], 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
    for index in range(count):
        name = 'input-%03d.txt' % index
        block = rng.randbytes(32768).hex().encode('ascii')
        data = (block[:32768] + b'\nneedle\n' + block[32768:] + b'\n') * 16
        path = root / name
        path.write_bytes(data)
        archive.write(path, name)
'@ | & $Python -B - $source $archive $Files
    if ($LASTEXITCODE -ne 0) { throw 'Python fixture generation failed' }
    $paths = @(Get-ChildItem -LiteralPath $source -File | Sort-Object Name | ForEach-Object { $_.FullName })
    $measurements = @{}
    $widths = @($Workers.Split(',') | ForEach-Object { [int]$_ })
    foreach ($tool in @('grep', 'unzip')) {
        $referenceOutput = $null
        foreach ($width in $widths) {
            $samples = @()
            for ($trial = 0; $trial -le $Repetitions; ++$trial) {
                $start = [Diagnostics.ProcessStartInfo]::new()
                $start.FileName = if ($tool -eq 'grep') { $grep } else { $unzip }
                $start.Arguments = if ($tool -eq 'grep') { '-c needle ' + (($paths | ForEach-Object { '"' + $_ + '"' }) -join ' ') } else { '-t "' + $archive + '"' }
                $start.WorkingDirectory = $repoRoot
                $start.UseShellExecute = $false
                $start.CreateNoWindow = $true
                $start.RedirectStandardOutput = $true
                $start.RedirectStandardError = $true
                $start.EnvironmentVariables["NEWOS_$($tool.ToUpperInvariant())_WORKERS"] = [string]$width
                if ($ProfileDir -ne '') {
                    [void][IO.Directory]::CreateDirectory($ProfileDir)
                    $start.EnvironmentVariables['NEWOS_PROFILE'] = Join-Path $ProfileDir ("$tool-$width-$trial.trace")
                    $start.EnvironmentVariables['NEWOS_PROFILE_MAX_EVENTS'] = '250000'
                }
                $clock = [Diagnostics.Stopwatch]::StartNew()
                $process = [Diagnostics.Process]::Start($start)
                try {
                    $output = $process.StandardOutput.ReadToEnd()
                    $errorText = $process.StandardError.ReadToEnd()
                    $process.WaitForExit()
                    $clock.Stop()
                    if ($process.ExitCode -ne 0) { throw "$tool failed: $errorText" }
                    $lines = @($output.Trim().Split("`n") | Where-Object { $_ -ne '' })
                    if ($tool -eq 'grep') {
                        if ($lines.Count -ne $Files -or @($lines | Where-Object { $_ -notmatch ':16\s*$' }).Count -ne 0) { throw 'grep output mismatch' }
                    } elseif ($lines.Count -ne 1 -or $lines[0] -notmatch '^No errors detected') {
                        throw 'unzip test output mismatch'
                    }
                    if ($null -eq $referenceOutput) { $referenceOutput = $output }
                    if ($output -cne $referenceOutput) { throw "$tool output changed at $width workers" }
                    if ($trial -gt 0) { $samples += $clock.Elapsed.TotalMilliseconds }
                } finally {
                    $process.Dispose()
                }
            }
            $measurements["$tool-$width"] = @($samples | Sort-Object)[[int][math]::Floor($samples.Count / 2)]
        }
    }
    'files={0} archive_bytes={1}' -f $Files, (Get-Item $archive).Length
    'workers grep_ms unzip_test_ms'
    foreach ($width in $widths) {
        '{0,7} {1,7:F1} {2,13:F1}' -f $width, $measurements["grep-$width"], $measurements["unzip-$width"]
    }
    $corrupt = Join-Path $scratch 'corrupt.zip'
    @'
import pathlib
import sys

data = bytearray(pathlib.Path(sys.argv[1]).read_bytes())
offset = data.index(b'PK\x01\x02')
data[offset + 16] ^= 1
pathlib.Path(sys.argv[2]).write_bytes(data)
'@ | & $Python -B - $archive $corrupt
    if ($LASTEXITCODE -ne 0) { throw 'Could not generate corrupt fixture' }
    $badStart = [Diagnostics.ProcessStartInfo]::new()
    $badStart.FileName = $unzip
    $badStart.Arguments = '-t "' + $corrupt + '"'
    $badStart.UseShellExecute = $false
    $badStart.CreateNoWindow = $true
    $badStart.RedirectStandardError = $true
    $badStart.RedirectStandardOutput = $true
    $badStart.EnvironmentVariables['NEWOS_UNZIP_WORKERS'] = '4'
    $badProcess = [Diagnostics.Process]::Start($badStart)
    try {
        $null = $badProcess.StandardOutput.ReadToEnd()
        $badError = $badProcess.StandardError.ReadToEnd()
        $badProcess.WaitForExit()
        if ($badProcess.ExitCode -eq 0 -or $badError -notmatch 'cannot test entry:') { throw 'unzip accepted a corrupted member' }
    } finally {
        $badProcess.Dispose()
    }
    $case = Invoke-Case $grep ('-ci NEEDLE "' + $paths[0] + '" "' + $paths[1] + '"') 'NEWOS_GREP_WORKERS'
    if ($case.ExitCode -ne 0 -or @($case.Output.Trim().Split("`n") | Where-Object { $_ -notmatch ':16\s*$' }).Count -ne 0) {
        throw 'case-insensitive grep count failed'
    }
    $case = Invoke-Case $grep ('-c needle "' + $paths[0] + '" "' + (Join-Path $scratch 'missing') + '"') 'NEWOS_GREP_WORKERS'
    if ($case.ExitCode -ne 1 -or $case.Output -notmatch ':16\s*$' -or $case.Error -notmatch 'cannot open') {
        throw 'grep missing-file behavior changed'
    }
    $extracted = Join-Path $scratch 'extracted'
    $case = Invoke-Case $unzip ('-d "' + $extracted + '" "' + $archive + '"') 'NEWOS_UNZIP_WORKERS'
    if ($case.ExitCode -ne 0) { throw "unzip extraction failed: $($case.Error)" }
    foreach ($path in $paths) {
        $restored = Join-Path $extracted (Split-Path $path -Leaf)
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $restored -Algorithm SHA256).Hash) {
            throw "unzip extraction mismatch: $path"
        }
    }
    if ($ProfileDir -ne '') {
        $profiler = (Resolve-Path (Join-Path $repoRoot (Join-Path $BuildDir 'profiler.exe'))).Path
        $nm = Get-Command llvm-nm -ErrorAction SilentlyContinue
        if (-not $nm) {
            $nm = Get-ChildItem "$env:LOCALAPPDATA/Microsoft/WinGet/Packages/MartinStorsjo.LLVM-MinGW.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/*/bin/llvm-nm.exe" | Select-Object -First 1
        }
        if (-not $nm) { throw 'llvm-nm is required for symbolized profiles' }
        foreach ($tool in @('grep', 'unzip')) {
            $symbols = Join-Path $ProfileDir "$tool.nm"
            & $nm.Source -n (Join-Path $repoRoot (Join-Path $BuildDir "$tool.exe")) | Set-Content -LiteralPath $symbols -Encoding ascii
            if ($LASTEXITCODE -ne 0) { throw "llvm-nm failed for $tool" }
            Write-Output "profile: $tool"
            & $profiler -m $symbols -n 12 --sort self (Join-Path $ProfileDir "$tool-$($widths[-1])-1.trace")
            if ($LASTEXITCODE -ne 0) { throw "profiler failed for $tool" }
        }
    }
    $completed = $true
} finally {
    if ($completed) { [IO.Directory]::Delete($scratch, $true) }
    else { Write-Warning "Fixture retained: $scratch" }
}