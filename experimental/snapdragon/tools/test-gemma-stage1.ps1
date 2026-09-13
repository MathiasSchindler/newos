param(
    [string]$Compiler = "clang",
    [string]$BuildDir = "experimental/snapdragon/build",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path

Push-Location $repoRoot
try {
    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot "build.ps1") -Compiler $Compiler -BuildDir $BuildDir
        if ($LASTEXITCODE -ne 0) { throw "Failed to build the Stage 1 probe" }
    }

    $probe = Join-Path $repoRoot "$BuildDir/npu_probe_builder.exe"
    if (-not (Test-Path -LiteralPath $probe)) { throw "Stage 1 probe not found: $probe" }

    $ErrorActionPreference = "Continue"
    $lines = & $probe --gemma-stage1 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    $output = $lines | Out-String
    if ($exitCode -ne 0) {
        throw "Stage 1 probe exited with $exitCode`n$output"
    }

    $checks = @(
        @("W4A16 projection", "(?s)Stage 1 W4A16 projection.*maximum absolute error \(micro-units\): 0.*result: supported and numerically accepted"),
        @("RMSNorm", "(?s)Stage 1 FP16 RMSNorm.*result: supported and numerically accepted"),
        @("gated GELU", "(?s)Stage 1 gated GELU composition.*result: supported and numerically accepted"),
        @("Gather", "(?s)Stage 1 Gather.*result: supported and exact"),
        @("Argmax and TopK", "(?s)Stage 1 Argmax and TopK.*result: supported and exact"),
        @("RotaryEmbedding", "(?s)Stage 1 RotaryEmbedding.*result: supported and exact"),
        @("direct GQA fallback", "(?s)Stage 1 GroupQueryAttention.*result: direct op unavailable; primitive composition required"),
        @("grouped causal attention", "(?s)Stage 1 grouped attention composition.*result: graph fallback grouped heads and causal masking accepted"),
        @("shared KV", "(?s)Stage 1 shared KV.*result: model-shaped shared KV registered, used, and exact")
    )
    foreach ($check in $checks) {
        if ($output -notmatch $check[1]) {
            throw "$($check[0]) capability result missing`n$output"
        }
        Write-Output "PASS $($check[0])"
    }
} finally {
    Pop-Location
}
