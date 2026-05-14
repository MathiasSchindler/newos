param(
    [string]$Compiler = "clang",
    [string]$Make = "make",
    [string]$MsysRoot = "C:\msys64"
)

$ErrorActionPreference = "Stop"

function Find-CommandPath([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    return $null
}

function Convert-ToMsysPath([string]$Path) {
    $converted = $Path -replace "\\", "/"
    if ($converted -match "^([A-Za-z]):/(.*)$") {
        return "/$($Matches[1].ToLower())/$($Matches[2])"
    }
    return $converted
}

$compilerPath = Find-CommandPath $Compiler
$msysCompiler = $null
if (-not $compilerPath) {
    $llvmClang = "C:\Program Files\LLVM\bin\clang.exe"
    $msysClang = Join-Path $MsysRoot "ucrt64\bin\clang.exe"
    if (Test-Path $llvmClang) {
        $compilerPath = $llvmClang
    } elseif (Test-Path $msysClang) {
        $compilerPath = $msysClang
        $msysCompiler = "/ucrt64/bin/clang"
    }
}

$makePath = Find-CommandPath $Make
$makeIsMsys = $false
if ($makePath) {
    $makeFullPath = [System.IO.Path]::GetFullPath($makePath)
    $msysFullPath = [System.IO.Path]::GetFullPath($MsysRoot)
    $makeIsMsys = $makeFullPath.StartsWith($msysFullPath, [System.StringComparison]::OrdinalIgnoreCase)
}

if ($makePath -and -not $makeIsMsys) {
    if (-not $compilerPath) {
        throw "Could not find '$Compiler' on PATH. Install LLVM/Clang for Windows or pass -Compiler <path>."
    }
    & $makePath freestanding-windows WINDOWS_TARGET_CC="$compilerPath"
    exit $LASTEXITCODE
}

$msysBash = Join-Path $MsysRoot "usr\bin\bash.exe"
if (-not (Test-Path $msysBash)) {
    throw "Could not find '$Make' on PATH or '$msysBash'. The current Makefile still needs make plus a POSIX shell as build drivers; the produced binaries do not depend on them."
}

$repo = Convert-ToMsysPath (Get-Location).Path

$escapedRepo = $repo -replace "'", "'\''"
$compilerForMsys = if ($msysCompiler) { $msysCompiler } elseif ($compilerPath) { Convert-ToMsysPath $compilerPath } else { $Compiler }
$escapedCompiler = $compilerForMsys -replace "'", "'\''"

& $msysBash -lc "export PATH=/ucrt64/bin:/usr/bin:/bin:`$PATH; cd '$escapedRepo' && make freestanding-windows WINDOWS_TARGET_CC='$escapedCompiler'"
exit $LASTEXITCODE
