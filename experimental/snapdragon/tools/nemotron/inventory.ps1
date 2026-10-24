param(
    [string]$DataDir = 'D:\newos-data\nemotron-asr',
    [string]$QnnDir = '',
    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
if (-not $QnnDir) { $QnnDir = Join-Path $PSScriptRoot '..\..\build' }
$output = Join-Path $DataDir 'target'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$hardware = Join-Path $output 'hardware.txt'
if (Test-Path -LiteralPath $hardware) { throw "Inventory already exists: $hardware" }
$text = & (Join-Path $PSScriptRoot '..\shared\inventory.ps1') -IncludePowerState | Out-String
[IO.File]::WriteAllText($hardware, $text, (New-Object Text.UTF8Encoding($false)))
$runtime = Join-Path $QnnDir 'qnn-runtime-version.txt'
if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) { throw "Missing QNN runtime lock: $runtime" }
Copy-Item -LiteralPath $runtime -Destination (Join-Path $output 'qnn-runtime-version.txt')
$driverStore = Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository'
$driverArguments = @()
foreach ($rpc in (Get-ChildItem -LiteralPath $driverStore -Recurse -File -Filter 'libcdsprpc.dll')) {
    $driverArguments += @('--driver-dir', $rpc.DirectoryName)
}
& $Python -B (Join-Path $PSScriptRoot 'nemotron_inventory.py') --qnn-dir $QnnDir --output (Join-Path $output 'dlls.json') @driverArguments
if ($LASTEXITCODE -ne 0) { throw 'QNN DLL inventory failed' }
Write-Output "Recorded target inventory at $output"
