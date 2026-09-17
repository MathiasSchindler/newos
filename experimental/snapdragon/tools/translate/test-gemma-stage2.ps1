param(
    [string]$Clang = "clang",
    [string]$Python = "python"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path

Push-Location $repoRoot
try {
    $buildDir = "experimental/snapdragon/build"
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    $descriptorTest = Join-Path $buildDir "test-gemma-model.exe"
    & $Clang -std=c11 -Wall -Wextra -Werror -ffreestanding `
        experimental/snapdragon/src/apps/translate/gemma_model.c `
        experimental/snapdragon/src/apps/translate/tests/test-gemma-model.c `
        -o $descriptorTest
    if ($LASTEXITCODE -ne 0) { throw "TranslateGemma descriptor compilation failed." }
    & $descriptorTest
    if ($LASTEXITCODE -ne 0) { throw "TranslateGemma descriptor test failed." }

    & $Python experimental/snapdragon/tools/translate/test-translategemma-stage2.py
    if ($LASTEXITCODE -ne 0) { throw "TranslateGemma checkpoint validation tests failed." }

    $null = [scriptblock]::Create(
        (Get-Content -LiteralPath experimental/snapdragon/tools/translate/fetch-translategemma.ps1 -Raw)
    )
    Write-Output "TranslateGemma Stage 2 tests passed."
} finally {
    Pop-Location
}