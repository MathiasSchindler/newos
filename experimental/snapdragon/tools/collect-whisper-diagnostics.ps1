param(
    [string]$WavPath = 'experimental/snapdragon/data/bundestag-hearing-5min-16k-mono-f32.wav',
    [string]$ProbePath = 'experimental/snapdragon/build/diagnostics-candidate/npu_probe.exe',
    [string]$OutputDirectory = 'experimental/snapdragon/data/medium-diagnostics/long',
    [ValidateRange(1, 10)][int]$Repetitions = 3,
    [string]$Python = 'experimental/snapdragon/build/calibration-venv/Scripts/python.exe'
)

$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$ProbePath = [IO.Path]::GetFullPath($ProbePath)
$WavPath = [IO.Path]::GetFullPath($WavPath)
$Python = [IO.Path]::GetFullPath($Python)
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$inventory = [ordered]@{
    timestamp = [DateTime]::UtcNow.ToString('o')
    os = [Environment]::OSVersion.VersionString
    processors = [Environment]::ProcessorCount
    power_scheme = (& powercfg /getactivescheme | Out-String).Trim()
    wpr = [bool](Get-Command wpr.exe -ErrorAction SilentlyContinue)
    elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    cpu_stack_capture = 'Not collected by this non-elevating harness; use WPR from an administrator terminal.'
    runtime_hashes = @(Get-ChildItem (Split-Path $ProbePath) -File | Where-Object Extension -in '.exe','.dll','.so' | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash)
    context_hash = Get-FileHash 'experimental/snapdragon/build/whisper-medium-m1c-encoder-fp16-l24.qnnctx' -Algorithm SHA256 | Select-Object Path,Hash
}
try { $inventory.battery = @(Get-CimInstance Win32_Battery | Select-Object BatteryStatus,EstimatedChargeRemaining) }
catch { $inventory.battery_error = $_.Exception.Message }
try { $inventory.thermal = @(Get-CimInstance -Namespace root/wmi -ClassName MSAcpi_ThermalZoneTemperature | Select-Object InstanceName,CurrentTemperature) }
catch { $inventory.thermal_error = $_.Exception.Message }
$inventory | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $OutputDirectory 'inventory.json')
$results = [Collections.Generic.List[object]]::new()
$referenceHash = $null
for ($repetition = 1; $repetition -le $Repetitions; ++$repetition) {
    $modes = if ($repetition % 2) { @('off','trace','basic') } else { @('basic','trace','off') }
    foreach ($mode in $modes) {
        $directory = Join-Path $OutputDirectory ('run-{0:D2}-{1}' -f $repetition,$mode)
        & (Join-Path $PSScriptRoot 'profile-whisper.ps1') -Model medium -DecoderOffload 'fused,self,logits' `
            -Diagnostics $mode -WavPath $WavPath -ProbePath $ProbePath -OutputDirectory $directory
        $summary = Get-Content (Join-Path $directory 'medium-summary.json') -Raw | ConvertFrom-Json
        if ($summary.exit_code -ne 0 -or !$summary.transcript_sha256) { throw "Run failed: $directory" }
        if (!$referenceHash) { $referenceHash = $summary.transcript_sha256 }
        if ($summary.transcript_sha256 -ne $referenceHash) { throw "Transcript changed: $directory" }
        if (Select-String -Path (Join-Path $directory 'medium.stderr.txt') -Pattern '<E>') { throw "QNN errors: $directory" }
        if ($mode -ne 'off') {
            & $Python (Join-Path $PSScriptRoot 'analyze-whisper-diagnostics.py') (Join-Path $directory 'diagnostics.csv')
            if ($LASTEXITCODE -ne 0) { throw "Diagnostics invalid: $directory" }
        }
        $summary | Add-Member -NotePropertyName repetition -NotePropertyValue $repetition
        $results.Add($summary)
        $results | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $OutputDirectory 'runs.json')
        Write-Output "PASS Medium repetition $repetition $mode"
    }
}