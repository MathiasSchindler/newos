param(
    [string]$DataDir = 'D:\newos-data\nemotron-asr',
    [string]$OutputDir = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$data = (Resolve-Path -LiteralPath $DataDir).Path
if (-not $OutputDir) { $OutputDir = Join-Path $data ('m4\acceptance-' + [guid]::NewGuid().ToString('N')) }
if (Test-Path -LiteralPath $OutputDir) { throw 'M4 evidence directory must not exist' }
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDir).Path
$python = Join-Path $repo 'experimental\snapdragon\build\gemma-oracle-x64\python.exe'
$package = Join-Path $data 'nemotron-runtime-m4.nmp'
$graphs = Join-Path $data 'm4\packed-decoder-graphs'
$approved = Join-Path $data 'm1\fastrpc-fp32-pad-v17\manifest.json'
$beforeApproved = (Get-FileHash -LiteralPath $approved -Algorithm SHA256).Hash
Push-Location $repo
try {
    if (-not (Test-Path -LiteralPath $package)) {
        & $python -B "$PSScriptRoot\nemotron_m4.py" --data-dir $data --deps-dir "$data\reference-packages" `
            export --output $package --graphs $graphs
        if ($LASTEXITCODE) { throw 'M4 export failed' }
    }
    & "$PSScriptRoot\build.ps1" -Test
    $exe = Join-Path $repo 'experimental\snapdragon\build\nemotron-asr\nemotron-asr.exe'
    $beforeExe = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    $beforePackage = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
    & $python -B "$PSScriptRoot\nemotron_m4.py" --data-dir $data --deps-dir "$data\reference-packages" `
        numerical --graphs $graphs --output "$output\numerical"
    if ($LASTEXITCODE) { throw 'M4 carried decoder numerical parity failed' }
    & $python -B "$PSScriptRoot\nemotron_m4.py" --data-dir $data --deps-dir "$data\reference-packages" `
        quality --executable $exe --package $package --output "$output\native"
    if ($LASTEXITCODE) { throw 'M4 full quality/offload gates failed' }
    & $python -B "$PSScriptRoot\nemotron_m4.py" --data-dir $data --deps-dir "$data\reference-packages" `
        benchmark --executable $exe --package $package --output "$output\speed"
    if ($LASTEXITCODE) { throw 'M4 end-to-end speed regressed' }
    & "$PSScriptRoot\build.ps1" -Clean
    & "$PSScriptRoot\build.ps1" -Test
    if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $beforeExe -or
        (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ne $beforePackage -or
        (Get-FileHash -LiteralPath $approved -Algorithm SHA256).Hash -ne $beforeApproved) {
        throw 'M4 clean/rebuild changed runtime or approved baseline'
    }
    & $python -B "$PSScriptRoot\nemotron_m3.py" --data-dir $data --deps-dir "$data\reference-packages" `
        audit --executable $exe --package $package --output "$output\restored" --fixtures tail
    if ($LASTEXITCODE) { throw 'Restored M4 model failed tail parity' }
    $report = Get-Content -LiteralPath "$output\native\report.json" -Raw | ConvertFrom-Json
    $report.status = 'm4_accepted'
    $report | Add-Member -NotePropertyName numerical -NotePropertyValue (Get-Content "$output\numerical\report.json" -Raw | ConvertFrom-Json)
    $report | Add-Member -NotePropertyName paired_speed -NotePropertyValue (Get-Content "$output\speed\report.json" -Raw | ConvertFrom-Json)
    $report | Add-Member -NotePropertyName lifecycle -NotePropertyValue @{
        passed=$true;clean_rebuild_byte_exact=$true;restored_tail_parity=$true;approved_m1_manifest_preserved=$true
    }
    [IO.File]::WriteAllText((Join-Path $output 'report.json'), (($report | ConvertTo-Json -Depth 24) + "`n"),
        (New-Object Text.UTF8Encoding($false)))
    Write-Output "PASS M4 acceptance: $output\report.json"
} finally {
    Pop-Location
}
