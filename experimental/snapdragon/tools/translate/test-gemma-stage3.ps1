param(
    [string]$Clang = "clang",
    [string]$Python = "experimental/snapdragon/build/calibration-venv/Scripts/python.exe"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path

Push-Location $repoRoot
try {
    $buildDir = "experimental/snapdragon/build"
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    $artifactTest = Join-Path $buildDir "test-gemma-artifact.exe"
    & $Clang -std=c11 -Wall -Wextra -Werror -ffreestanding `
        -Isrc/shared -Iexperimental/snapdragon/src/apps/translate `
        experimental/snapdragon/src/apps/translate/gemma_model.c `
        experimental/snapdragon/src/apps/translate/gemma_artifact.c `
        src/shared/crypto/sha256.c `
        experimental/snapdragon/src/apps/translate/tests/test-gemma-artifact.c `
        -o $artifactTest
    if ($LASTEXITCODE -ne 0) { throw "TranslateGemma artifact compilation failed." }
    & $artifactTest
    if ($LASTEXITCODE -ne 0) { throw "TranslateGemma artifact tests failed." }

    & $Python experimental/snapdragon/tools/translate/test-translategemma-stage3.py
    if ($LASTEXITCODE -ne 0) { throw "TranslateGemma converter tests failed." }
    Write-Output "TranslateGemma Stage 3 tests passed."
} finally {
    Pop-Location
}