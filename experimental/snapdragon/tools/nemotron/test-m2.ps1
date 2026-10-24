param(
    [string]$DataDir = 'D:\newos-data\nemotron-asr',
    [string]$OutputDir = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$data = (Resolve-Path -LiteralPath $DataDir).Path
if (-not $OutputDir) { $OutputDir = Join-Path $data ('m2\acceptance-' + [guid]::NewGuid().ToString('N')) }
if (Test-Path -LiteralPath $OutputDir) { throw 'M2 evidence directory must not exist' }
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDir).Path
$python = Join-Path $repo 'experimental\snapdragon\build\gemma-oracle-x64\python.exe'
$package = Join-Path $data 'nemotron-runtime-m2.nmp'
$approved = Join-Path $data 'm1\fastrpc-fp32-pad-v17\manifest.json'
$beforeApproved = (Get-FileHash -LiteralPath $approved -Algorithm SHA256).Hash
Push-Location $repo
try {
    if (-not (Test-Path -LiteralPath $package)) {
        & $python -B "$PSScriptRoot\nemotron_m2.py" --data-dir $data export --output $package
        if ($LASTEXITCODE) { throw 'M2 export failed' }
    }
    & "$PSScriptRoot\build.ps1" -Test
    $source = 'experimental\snapdragon\src\apps\nemotron'
    $library = Join-Path $output 'frontend-reference.dll'
    & clang --target=x86_64-w64-windows-gnu -shared -O2 -ffp-contract=off -std=c11 `
        -Wall -Wextra -Wpedantic -Werror -Isrc\shared -Iexperimental\snapdragon\src\shared `
        "$source\nemotron_frontend.c" "$source\nemotron_wav.c" "$source\nemotron_reader.c" `
        "$source\tests\frontend_reference.c" "$source\nemotron_tokenizer.c" 'src\shared\math.c' 'src\shared\runtime\string.c' `
        'src\shared\runtime\parse.c' 'src\shared\runtime\unicode_utf8.c' 'src\shared\crypto\sha256.c' `
        '-Wl,--export-all-symbols' -o $library
    if ($LASTEXITCODE) { throw 'M2 development reference compilation failed' }
    & $python -B "$PSScriptRoot\test_nemotron_m2.py" --library $library --data-dir $data `
        --deps-dir "$data\reference-packages"
    if ($LASTEXITCODE) { throw 'M2 development/negative tests failed' }
    $exe = Join-Path $repo 'experimental\snapdragon\build\nemotron-asr\nemotron-frontend.exe'
    $beforeExe = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    $beforePackage = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
    $audit = Join-Path $output 'native'
    & $python -B "$PSScriptRoot\nemotron_m2.py" --data-dir $data --deps-dir "$data\reference-packages" `
        audit --executable $exe --package $package --output $audit
    if ($LASTEXITCODE) { throw 'Full native M2 acceptance failed' }
    & "$PSScriptRoot\build.ps1" -Clean
    if (Test-Path -LiteralPath $package) {
        if ((Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ne $beforePackage) {
            throw 'Isolated clean changed runtime package'
        }
    } else { throw 'Isolated clean removed runtime package' }
    & "$PSScriptRoot\build.ps1" -Test
    $afterExe = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    if ($beforeExe -ne $afterExe) { throw 'Frontend clean/rebuild was not byte-reproducible' }
    if ((Get-FileHash -LiteralPath $approved -Algorithm SHA256).Hash -ne $beforeApproved) {
        throw 'M2 changed immutable approved M1 manifest'
    }
    & $exe check $package
    if ($LASTEXITCODE) { throw 'Restored runtime package validation failed' }
    $report = Get-Content -LiteralPath "$audit\report.json" -Raw | ConvertFrom-Json
    $report.status = 'm2_accepted'
    $report | Add-Member -NotePropertyName lifecycle -NotePropertyValue @{
        passed = $true
        frontend_sha256 = $afterExe.ToLowerInvariant()
        package_preserved = $true
        approved_m1_manifest_preserved = $true
        isolated_clean_rebuild_byte_exact = $true
    }
    [IO.File]::WriteAllText((Join-Path $output 'report.json'), (($report | ConvertTo-Json -Depth 16) + "`n"),
        (New-Object Text.UTF8Encoding($false)))
    Write-Output "PASS M2 acceptance: $output\report.json"
} finally {
    Pop-Location
}
