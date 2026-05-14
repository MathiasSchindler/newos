$MsysBin = 'C:\msys64\usr\bin'

if (-not (Test-Path (Join-Path $MsysBin 'msys-2.0.dll'))) {
    throw "MSYS runtime not found at $MsysBin"
}

$pathEntries = $env:PATH -split ';' | Where-Object { $_ -ne '' }
if ($pathEntries -notcontains $MsysBin) {
    $env:PATH = $MsysBin + ';' + $env:PATH
}

Write-Output "MSYS hosted runtime enabled: $MsysBin"