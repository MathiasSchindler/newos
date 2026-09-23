param(
    [ValidateSet('Hmx', 'Encoder', 'Cpu')]
    [string]$Mode = 'Hmx',
    [ValidateSet('Tiny', 'Base')]
    [string]$Model = 'Tiny',
    [switch]$RequireBatch,
    [switch]$RequireGrouped,
    [string]$WavPath = 'experimental/snapdragon/models/calibration/fleurs/de_de-1586.wav',
    [string]$ModelPath = '',
    [string]$OutputDirectory = 'experimental/snapdragon/tests/tmp/whisper-hmx-audio'
)

$ErrorActionPreference = 'Stop'
if ($RequireBatch -and $Mode -eq 'Cpu') { throw 'RequireBatch needs an HMX mode' }
if ($RequireGrouped -and $Mode -eq 'Cpu') { throw 'RequireGrouped needs an HMX mode' }
if ($Model -eq 'Base' -and $Mode -eq 'Hmx') { throw 'Base requires Encoder or Cpu mode' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
$build = Join-Path $repo 'experimental/snapdragon/build'
$exe = Join-Path $build 'whisper-direct/whisper-cli.exe'
$frontend = Join-Path $repo 'experimental/snapdragon/models/whisper-tiny/frontend-fp16'
$wav = (Resolve-Path (Join-Path $repo $WavPath)).Path
if (-not $ModelPath) {
    $ModelPath = 'experimental/snapdragon/build/whisper-direct/' + $Model.ToLowerInvariant() + '-checked.wti'
}
$indexedPath = (Resolve-Path (Join-Path $repo $ModelPath)).Path
$output = Join-Path $repo $OutputDirectory
$drivers = @(Get-ChildItem "$env:SystemRoot/System32/DriverStore/FileRepository/qcnspmcdm8380.inf_arm64_*/libcdsprpc.dll")
if ($drivers.Count -ne 1) { throw 'Expected exactly one installed MCDM driver' }
if (-not (Test-Path $exe) -or
    ($Mode -ne 'Cpu' -and
     (-not (Test-Path (Join-Path $build 'fastrpc_probe_skel.so')) -or
      -not (Test-Path (Join-Path $build 'fastrpc_probe_skel.cat'))))) {
    throw 'Build Tiny and stage the signed HMX module and catalog in the build directory'
}
New-Item -ItemType Directory -Force -Path $output | Out-Null
$info = New-Object System.Diagnostics.ProcessStartInfo
$info.FileName = $exe
$info.WorkingDirectory = $build
$command = if ($Model -eq 'Base' -and $Mode -eq 'Encoder') { '--transcribe-base-hmx-encoder' }
elseif ($Model -eq 'Base') { '--transcribe-base' }
elseif ($Mode -eq 'Encoder') { '--transcribe-hmx-encoder' } elseif ($Mode -eq 'Hmx') {
    '--transcribe-hmx'
} else { '--transcribe' }
$info.Arguments = $command +
    ' "' + $wav + '" "' + $indexedPath + '" "' + $frontend + '"'
if ($Mode -ne 'Cpu') { $info.Arguments += ' "' + $drivers[0].FullName + '"' }
$info.UseShellExecute = $false
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$info.StandardOutputEncoding = [Text.Encoding]::UTF8
$info.StandardErrorEncoding = [Text.Encoding]::UTF8
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WhisperPowerTiming {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint SetThreadExecutionState(uint flags);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool QueryUnbiasedInterruptTime(out ulong ticks);
}
'@
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $info
$awakeStart = [UInt64]0
if (-not [WhisperPowerTiming]::QueryUnbiasedInterruptTime([ref]$awakeStart)) {
    throw 'Cannot read sleep-excluding interrupt time'
}
if (-not [WhisperPowerTiming]::SetThreadExecutionState([uint32]2147483649)) {
    throw 'Cannot request continuous system wakefulness'
}
try {
$timer = [Diagnostics.Stopwatch]::StartNew()
if (-not $process.Start()) { throw 'Unable to launch HMX transcription' }
$stdout = $process.StandardOutput.ReadToEndAsync()
$stderr = $process.StandardError.ReadToEndAsync()
$process.WaitForExit()
$timer.Stop()
$awakeEnd = [UInt64]0
if (-not [WhisperPowerTiming]::QueryUnbiasedInterruptTime([ref]$awakeEnd)) {
    throw 'Cannot read sleep-excluding interrupt time after transcription'
}
$text = $stdout.Result
$errors = $stderr.Result
$result = [ordered]@{
    mode = $Mode
    exit_code = $process.ExitCode
    wall_seconds = $timer.Elapsed.TotalSeconds
    awake_seconds = ($awakeEnd - $awakeStart) / 10000000.0
    cpu_seconds = $process.TotalProcessorTime.TotalSeconds
    hmx_submissions = 0
    hmx_batched = 0
    hmx_grouped = 0
    hmx_invoke_milliseconds = 0
    hmx_invoke_widths = [ordered]@{}
    transcript = ''
}
$match = [regex]::Match($text, '(?m)^hmx\.projection\.submissions=(\d+)\r?$')
if ($match.Success) { $result.hmx_submissions = [int]$match.Groups[1].Value }
$match = [regex]::Match($text, '(?m)^hmx\.projection\.batched=(\d+)\r?$')
if ($match.Success) { $result.hmx_batched = [int]$match.Groups[1].Value }
$match = [regex]::Match($text, '(?m)^hmx\.projection\.grouped=(\d+)\r?$')
if ($match.Success) { $result.hmx_grouped = [int]$match.Groups[1].Value }
$match = [regex]::Match($text, '(?m)^hmx\.invoke\.milliseconds=(\d+)\r?$')
if ($match.Success) { $result.hmx_invoke_milliseconds = [int]$match.Groups[1].Value }
foreach ($match in [regex]::Matches($text, '(?m)^hmx\.invoke\.width(384|512|1536|2048)\.(calls|milliseconds)=(\d+)\r?$')) {
    $width = $match.Groups[1].Value
    if (-not $result.hmx_invoke_widths.Contains($width)) {
        $result.hmx_invoke_widths[$width] = [ordered]@{ calls = 0; milliseconds = 0 }
    }
    $result.hmx_invoke_widths[$width][$match.Groups[2].Value] = [int]$match.Groups[3].Value
}
$result.transcript = ($text -replace '(?m)^hmx\.(projection\.(submissions|batched|grouped)|invoke\.(milliseconds|width\d+\.(calls|milliseconds)))=\d+\r?\n?', '').Trim()
$utf8 = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $output 'stdout.txt'), $text, $utf8)
[IO.File]::WriteAllText((Join-Path $output 'stderr.txt'), $errors, $utf8)
[IO.File]::WriteAllText((Join-Path $output 'result.json'), ($result | ConvertTo-Json -Depth 5) + "`n", $utf8)
Write-Output ('Transcription exit={0} submissions={1} batched={2} wall={3:F2}s cpu={4:F2}s' -f
    $result.exit_code,$result.hmx_submissions,$result.hmx_batched,$result.wall_seconds,$result.cpu_seconds)
if ($result.wall_seconds - $result.awake_seconds -gt 5) {
    throw ('System slept during transcription; discard timing in ' + $output)
}
if ($result.exit_code -ne 0 -or -not $result.transcript -or
    ($Mode -ne 'Cpu' -and $result.hmx_submissions -le 0) -or
    ($RequireBatch -and $result.hmx_batched -ne 1) -or
    ($RequireGrouped -and $result.hmx_grouped -ne 1)) {
    throw ('HMX transcription failed; inspect ' + $output)
}
} finally {
    [void][WhisperPowerTiming]::SetThreadExecutionState([uint32]2147483648)
    $process.Dispose()
}