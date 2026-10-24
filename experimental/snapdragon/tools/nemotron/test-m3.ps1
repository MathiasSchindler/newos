param(
    [string]$DataDir = 'D:\newos-data\nemotron-asr',
    [string]$OutputDir = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$data = (Resolve-Path -LiteralPath $DataDir).Path
if (-not $OutputDir) { $OutputDir = Join-Path $data ('m3\acceptance-' + [guid]::NewGuid().ToString('N')) }
if (Test-Path -LiteralPath $OutputDir) { throw 'M3 evidence directory must not exist' }
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDir).Path
$python = Join-Path $repo 'experimental\snapdragon\build\gemma-oracle-x64\python.exe'
$package = Join-Path $data 'nemotron-runtime-m3.nmp'
$approved = Join-Path $data 'm1\fastrpc-fp32-pad-v17\manifest.json'
$beforeApproved = (Get-FileHash -LiteralPath $approved -Algorithm SHA256).Hash
Push-Location $repo
try {
    if (-not (Test-Path -LiteralPath $package)) {
        & $python -B "$PSScriptRoot\nemotron_m3.py" --data-dir $data export --output $package
        if ($LASTEXITCODE) { throw 'M3 export failed' }
    }
    & "$PSScriptRoot\build.ps1" -Test
    $source = 'experimental\snapdragon\src\apps\nemotron'
    $library = Join-Path $output 'decoder-reference.dll'
    & clang --target=x86_64-w64-windows-gnu -shared -O2 -ffp-contract=off -std=c11 `
        -Wall -Wextra -Wpedantic -Werror -Isrc\shared -Iexperimental\snapdragon\src\shared `
        "$source\nemotron_frontend.c" "$source\nemotron_wav.c" "$source\nemotron_reader.c" `
        "$source\nemotron_tokenizer.c" "$source\tests\frontend_reference.c" 'src\shared\math.c' `
        'src\shared\runtime\string.c' 'src\shared\runtime\parse.c' 'src\shared\runtime\unicode_utf8.c' `
        'src\shared\crypto\sha256.c' '-Wl,--export-all-symbols' -o $library
    if ($LASTEXITCODE) { throw 'M3 developer reference build failed' }
    & $python -B "$PSScriptRoot\test_nemotron_m3.py" --library $library --data-dir $data `
        --deps-dir "$data\reference-packages"
    if ($LASTEXITCODE) { throw 'Native tokenizer/source compatibility tests failed' }
    & $python -B "$PSScriptRoot\test_nemotron_m2.py" --library $library --data-dir $data `
        --deps-dir "$data\reference-packages"
    if ($LASTEXITCODE) { throw 'M2 regression tests failed' }
    $exe = Join-Path $repo 'experimental\snapdragon\build\nemotron-asr\nemotron-asr.exe'
    $beforeExe = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    $beforePackage = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
    $audit = Join-Path $output 'native'
    & $python -B "$PSScriptRoot\nemotron_m3.py" --data-dir $data --deps-dir "$data\reference-packages" `
        audit --executable $exe --package $package --output $audit
    if ($LASTEXITCODE) { throw 'Full native M3 WAV acceptance failed' }
    & "$PSScriptRoot\build.ps1" -Clean
    & "$PSScriptRoot\build.ps1" -Test
    $afterExe = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    if ($beforeExe -ne $afterExe) { throw 'ASR clean/rebuild was not byte-reproducible' }
    if ((Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ne $beforePackage -or
        (Get-FileHash -LiteralPath $approved -Algorithm SHA256).Hash -ne $beforeApproved) {
        throw 'Clean/rebuild changed runtime index or approved v17 baseline'
    }
    & $python -B "$PSScriptRoot\nemotron_m3.py" --data-dir $data --deps-dir "$data\reference-packages" `
        audit --executable $exe --package $package --output "$output\restored" --fixtures tail
    if ($LASTEXITCODE) { throw 'Restored WAV runtime failed exact tail parity' }
    $report = Get-Content -LiteralPath "$audit\report.json" -Raw | ConvertFrom-Json
    $report.status = 'm3_accepted'
    $report | Add-Member -NotePropertyName lifecycle -NotePropertyValue @{
        passed = $true
        asr_sha256 = $afterExe.ToLowerInvariant()
        package_preserved = $true
        approved_m1_manifest_preserved = $true
        clean_rebuild_byte_exact = $true
        restored_tail_token_text_parity = $true
    }
    [IO.File]::WriteAllText((Join-Path $output 'report.json'), (($report | ConvertTo-Json -Depth 16) + "`n"),
        (New-Object Text.UTF8Encoding($false)))
    Write-Output "PASS M3 acceptance: $output\report.json"
} finally {
    Pop-Location
}
