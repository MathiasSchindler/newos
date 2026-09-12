param(
    [string]$Compiler = "clang",
    [string]$BuildDir = "experimental/snapdragon/build"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$testRoot = Join-Path $repoRoot "tests/tmp/snapdragon-npu-probe"
$probePath = Join-Path $repoRoot "$BuildDir/npu_probe.exe"
$mockSource = Join-Path $PSScriptRoot "..\src\qnn_mock.c"

function Assert-Case {
    param(
        [string]$Name,
        [int]$ActualExit,
        [int]$ExpectedExit,
        [string]$Output,
        [string]$ExpectedPattern,
        [string]$ForbiddenPattern = ""
    )

    if ($ActualExit -ne $ExpectedExit) {
        throw "$Name exited with $ActualExit; expected $ExpectedExit`n$Output"
    }
    if ($Output -notmatch $ExpectedPattern) {
        throw "$Name did not match '$ExpectedPattern'`n$Output"
    }
    if ($ForbiddenPattern -and $Output -match $ForbiddenPattern) {
        throw "$Name unexpectedly matched '$ForbiddenPattern'`n$Output"
    }
    Write-Output "PASS $Name (exit $ActualExit)"
}

function Invoke-Probe {
    param([string]$Directory)

    Push-Location $Directory
    try {
        $lines = & .\npu_probe.exe 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    return @($exitCode, ($lines | Out-String))
}

Push-Location $repoRoot
try {
    & (Join-Path $PSScriptRoot "build.ps1") -Compiler $Compiler -BuildDir $BuildDir
    if ($LASTEXITCODE -ne 0) { throw "Failed to build npu_probe.exe" }

    $compilerCommand = Get-Command $Compiler -ErrorAction SilentlyContinue
    if (-not $compilerCommand) { throw "Could not find Clang: $Compiler" }
    $compilerPath = $compilerCommand.Source
    $mockFlags = @(
        "--target=aarch64-w64-windows-gnu", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Oz",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdlib", "-fuse-ld=lld", "-shared"
    )

    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $testRoot | Out-Null

    $noDllDirectory = Join-Path $testRoot "no-dll"
    New-Item -ItemType Directory -Force -Path $noDllDirectory | Out-Null
    Copy-Item -LiteralPath $probePath -Destination $noDllDirectory
    $result = Invoke-Probe $noDllDirectory
    Assert-Case "missing DLL" $result[0] 2 $result[1] "not loadable"

    $cases = @(
        @("missing symbol", "QNN_MOCK_NO_PROVIDER_EXPORT", 3, "getProviders: missing", ""),
        @("API mismatch", "QNN_MOCK_API_MISMATCH", 5, "No ABI-compatible QNN 2.32\+ provider found", ""),
        @("missing lifecycle", "QNN_MOCK_MISSING_LIFECYCLE", 6, "missing one or more required lifecycle or graph functions", ""),
        @("context failure cleanup", "QNN_MOCK_CONTEXT_FAILURE", 11, "(?s)contextCreate: 0x0000000000001234.*profileFree: 0x0000000000000000.*deviceFree: 0x0000000000000000.*backendFree: 0x0000000000000000.*logFree: 0x0000000000000000", "contextFree:"),
        @("graph create failure", "QNN_MOCK_GRAPH_CREATE_FAILURE", 17, "(?s)graphCreate: 0x0000000000002001.*contextFree: 0x0000000000000000", "graphAddNode:"),
        @("tensor create failure", "QNN_MOCK_TENSOR_FAILURE", 18, "(?s)tensorCreate input_a: 0x0000000000002002.*contextFree: 0x0000000000000000", "graphAddNode:"),
        @("add node failure", "QNN_MOCK_ADD_NODE_FAILURE", 21, "(?s)graphAddNode: 0x0000000000002003.*contextFree: 0x0000000000000000", "graphFinalize:"),
        @("finalize failure", "QNN_MOCK_FINALIZE_FAILURE", 22, "(?s)graphFinalize: 0x0000000000002004.*contextFree: 0x0000000000000000", "graphExecute:"),
        @("execute failure", "QNN_MOCK_EXECUTE_FAILURE", 23, "(?s)graphExecute: 0x0000000000002005.*contextFree: 0x0000000000000000", "output matches"),
        @("output mismatch", "QNN_MOCK_BAD_OUTPUT", 24, "output mismatch at 0: expected 11, got 12", "output matches"),
        @("graph success", "QNN_MOCK_SUCCESS", 0, "(?s)output matches CPU reference.*MatMul \[1,384\].*MatMul \[1500,384\] x \[384,384\].*MatMul \[1500,384\] x \[384,1536\].*MatMul \[1500,1536\] x \[1536,384\].*output matches scalar UINT8 GEMM reference.*contextFree: 0x0000000000000000", "output mismatch")
    )

    foreach ($case in $cases) {
        $name = $case[0]
        $define = $case[1]
        $caseDirectory = Join-Path $testRoot ($name -replace " ", "-")
        New-Item -ItemType Directory -Force -Path $caseDirectory | Out-Null
        Copy-Item -LiteralPath $probePath -Destination $caseDirectory
        & $compilerPath @mockFlags "-D$define" $mockSource -o (Join-Path $caseDirectory "QnnHtp.dll")
        if ($LASTEXITCODE -ne 0) { throw "Failed to build mock for $name" }

        $result = Invoke-Probe $caseDirectory
        Assert-Case $name $result[0] $case[2] $result[1] $case[3] $case[4]
    }
} finally {
    Pop-Location
}
