param(
    [string]$BuildDir = 'build/normal',
    [string]$PackedDir = 'build/packed'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Push-Location $repoRoot
try {
    $trace = Join-Path $repoRoot 'tests/tmp/windows-strace.trace'
    $jsonTrace = Join-Path $repoRoot 'tests/tmp/windows-strace.jsonl'
    $summary = Join-Path $repoRoot 'tests/tmp/windows-strace-summary.txt'
    $input = Join-Path $repoRoot 'tests/tmp/windows strace input.txt'
    [IO.File]::WriteAllText($input, 'windows strace smoke')

    & (Join-Path $BuildDir 'strace.exe') -T -o $trace -e open,read,write,close (Join-Path $BuildDir 'cat.exe') $input > $null
    if ($LASTEXITCODE -ne 0) { throw 'Normal Windows strace could not trace cat' }
    $text = [IO.File]::ReadAllText($trace)
    if ($text -notmatch 'open\(".*windows strace input\.txt"' -or $text -notmatch 'read\(' -or
        $text -notmatch 'write\(' -or $text -notmatch 'close\(' -or $text -notmatch '<[0-9]+\.[0-9]+ ms>') {
        throw 'Normal Windows strace omitted a platform I/O event or duration'
    }

    & (Join-Path $BuildDir 'strace.exe') --json -o $jsonTrace -e open (Join-Path $BuildDir 'cat.exe') $input > $null
    if ($LASTEXITCODE -ne 0) { throw 'Windows strace JSON capture failed' }
    $json = [IO.File]::ReadAllText($jsonTrace)
    if ($json -notmatch '"event":"syscall"' -or $json -notmatch '"name":"open"' -or
        $json -notmatch '"value":"[^"\r\n]*windows strace input\.txt"') {
        throw 'Windows strace JSON event lost the decoded path'
    }

    & (Join-Path $PackedDir 'strace.exe') -c -o $summary -e open,read,close (Join-Path $PackedDir 'cat.exe') $input > $null
    if ($LASTEXITCODE -ne 0) { throw 'Packed Windows strace could not trace cat' }
    $counts = [IO.File]::ReadAllText($summary)
    if ($counts -notmatch '(?m)^open 1 0 ' -or $counts -notmatch '(?m)^read [1-9][0-9]* 0 ' -or
        $counts -notmatch '(?m)^close 1 0 ') { throw 'Packed Windows strace summary omitted file I/O' }

    & (Join-Path $BuildDir 'strace.exe') cmd.exe /c exit 7 > $null
    if ($LASTEXITCODE -ne 7) { throw "Windows strace did not propagate the child exit code: $LASTEXITCODE" }

    foreach ($tool in @('grep', 'sha256sum', 'file')) {
        $arguments = @('LICENSE')
        if ($tool -eq 'grep') { $arguments = @('-n', 'CORPORATION', 'LICENSE') }
        $toolTrace = Join-Path $repoRoot "tests/tmp/windows-strace-$tool.trace"
        $toolOutput = Join-Path $repoRoot "tests/tmp/windows-strace-$tool.out"
        & (Join-Path $BuildDir 'strace.exe') -o $toolTrace -e open,read,write,close (Join-Path $BuildDir "$tool.exe") @arguments > $toolOutput
        if ($LASTEXITCODE -ne 0 -or (Get-Item $toolOutput).Length -eq 0) { throw "Traced $tool did not produce output" }
        $events = [IO.File]::ReadAllText($toolTrace)
        if ($events -notmatch 'open\("LICENSE"' -or $events -notmatch 'read\(' -or $events -notmatch 'write\(') {
            throw "Traced $tool omitted expected file I/O"
        }
        Write-Output ("PASS {0}: {1} trace events" -f $tool, @([IO.File]::ReadAllLines($toolTrace)).Count)
    }

    $copyPath = Join-Path $repoRoot 'tests/tmp/windows-strace-copy.txt'
    $copyTrace = Join-Path $repoRoot 'tests/tmp/windows-strace-cp.trace'
    if (Test-Path -LiteralPath $copyPath) { Remove-Item -LiteralPath $copyPath }
    & (Join-Path $BuildDir 'strace.exe') -o $copyTrace -e open,read,write,close (Join-Path $BuildDir 'cp.exe') LICENSE $copyPath
    if ($LASTEXITCODE -ne 0 -or (Get-FileHash LICENSE).Hash -ne (Get-FileHash $copyPath).Hash) {
        throw 'Traced cp did not preserve the file contents'
    }
    $copyEvents = [IO.File]::ReadAllText($copyTrace)
    if ($copyEvents -notmatch 'open\("LICENSE"' -or $copyEvents -notmatch 'open\("[^"\r\n]*windows-strace-copy\.txt"' -or
        $copyEvents -notmatch 'write\(') { throw 'Traced cp omitted copy I/O' }
    Write-Output ("PASS cp: {0} trace events, matching output hash" -f @([IO.File]::ReadAllLines($copyTrace)).Count)

    $missingTrace = Join-Path $repoRoot 'tests/tmp/windows-strace-missing.trace'
    try {
        $ErrorActionPreference = 'Continue'
        & (Join-Path $BuildDir 'strace.exe') -o $missingTrace -e open (Join-Path $BuildDir 'cat.exe') 'tests/tmp/windows-strace-missing-input' 2> $null
    } finally {
        $ErrorActionPreference = 'Stop'
    }
    if ($LASTEXITCODE -eq 0 -or [IO.File]::ReadAllText($missingTrace) -notmatch 'open\("[^"\r\n]*windows-strace-missing-input".* = -[1-9]') {
        throw 'Traced missing-file failure lost its negative open result'
    }
    Write-Output 'PASS Windows strace: native I/O, paths, timing, JSON, summary, and exit status'
} finally {
    Pop-Location
}