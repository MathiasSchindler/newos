param(
    [string]$Compiler = "C:\Program Files\LLVM\bin\clang.exe"
)

$ErrorActionPreference = "Stop"

if ($env:PROCESSOR_ARCHITECTURE -ne "ARM64") {
    Write-Output "SKIP: PE ARM64 linker execution test requires native ARM64 Windows"
    exit 0
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Push-Location $repoRoot
try {
    if (-not (Test-Path $Compiler)) {
        $command = Get-Command $Compiler -ErrorAction SilentlyContinue
        if (-not $command) { throw "Could not find Clang: $Compiler" }
        $Compiler = $command.Source
    }

    $compilerDir = Split-Path -Parent $Compiler
    $readobj = Join-Path $compilerDir "llvm-readobj.exe"
    if (-not (Test-Path $readobj)) { throw "Could not find llvm-readobj beside Clang" }

    & .\tests\windows\build-windows-freestanding.ps1 -Compiler $Compiler -TargetTriple aarch64-w64-windows-gnu -Tools linker
    if ($LASTEXITCODE -ne 0) { throw "Failed to build the Windows ARM64 linker" }

    $linker = "build\freestanding-windows-aarch64\linker.exe"
    $scratch = "tests\tmp\windows-pe-arm64-linker"
    New-Item -ItemType Directory -Force $scratch | Out-Null

    [IO.File]::WriteAllText((Join-Path $scratch "entry.c"), @'
__declspec(dllimport) void ExitProcess(unsigned int exit_code);
__declspec(dllimport) void Sleep(unsigned long milliseconds);
int selected_value(unsigned int index);
int associative_parent(void);

static volatile const char dead_marker[] = "PE_GC_DEAD_MARKER";

__attribute__((used, noinline))
static void dead_import_path(void) {
    if (dead_marker[0] != 0) Sleep(1);
}

void mainCRTStartup(void) {
    ExitProcess(selected_value(0U) == 17 && associative_parent() == 7 ? 0U : 1U);
}
'@)
    [IO.File]::WriteAllText((Join-Path $scratch "first.c"), @'
__attribute__((weak, noinline))
int selected_value(unsigned int index) {
    static const char *values[] = { "17", "23" };
    return values[index][0] == '1' ? 17 : 23;
}
'@)
    [IO.File]::WriteAllText((Join-Path $scratch "second.c"), @'
__attribute__((weak, noinline))
int selected_value(unsigned int index) {
    static const char *values[] = { "23", "17" };
    return values[index][0] == '2' ? 23 : 17;
}
'@)
    [IO.File]::WriteAllText((Join-Path $scratch "simple.c"), @'
__declspec(dllimport) void ExitProcess(unsigned int exit_code);

void mainCRTStartup(void) {
    ExitProcess(0U);
}
'@)
    [IO.File]::WriteAllText((Join-Path $scratch "associative.S"), @'
    .section .text$associative_parent,"xr",one_only,associative_parent
    .globl associative_parent
    .p2align 2
associative_parent:
    mov w0, #7
    ret

    .section .rdata$associative_child,"dr",associative,associative_parent
    .asciz "PE_ASSOCIATIVE_LIVE"
'@)

    $commonFlags = @(
        "--target=aarch64-w64-windows-gnu", "-std=c11", "-Oz", "-ffreestanding",
        "-fno-builtin", "-fno-stack-protector", "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables", "-ffunction-sections", "-fdata-sections"
    )
    foreach ($name in @("entry", "first", "second", "simple")) {
        & $Compiler @commonFlags -c (Join-Path $scratch "$name.c") -o (Join-Path $scratch "$name.obj")
        if ($LASTEXITCODE -ne 0) { throw "Failed to compile $name.c" }
    }
    & $Compiler --target=aarch64-w64-windows-gnu -c (Join-Path $scratch "associative.S") -o (Join-Path $scratch "associative.obj")
    if ($LASTEXITCODE -ne 0) { throw "Failed to compile associative.S" }

    $associativeSymbols = & $readobj --symbols (Join-Path $scratch "associative.obj") | Out-String
    if ($associativeSymbols -notmatch "Selection: Associative") {
        throw "Clang fixture did not emit the expected associative COMDAT"
    }

    $kernel32 = "src\platform\windows\imports\kernel32.def"
    & $linker --target=pe-arm64 --gc-sections -o (Join-Path $scratch "gc.exe") `
        (Join-Path $scratch "entry.obj") (Join-Path $scratch "first.obj") `
        (Join-Path $scratch "second.obj") (Join-Path $scratch "associative.obj") $kernel32
    if ($LASTEXITCODE -ne 0) { throw "PE ARM64 GC link failed" }
    & (Join-Path $scratch "gc.exe")
    if ($LASTEXITCODE -ne 0) { throw "PE ARM64 GC executable returned $LASTEXITCODE" }

    $imports = & $readobj --coff-imports (Join-Path $scratch "gc.exe") | Out-String
    if ($imports -notmatch "Symbol: ExitProcess" -or $imports -match "Symbol: Sleep") {
        throw "PE ARM64 GC retained the wrong import set"
    }
    $imageText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $scratch "gc.exe")))
    if ($imageText.Contains("PE_GC_DEAD_MARKER")) {
        throw "PE ARM64 GC retained dead section data"
    }
    if (-not $imageText.Contains("PE_ASSOCIATIVE_LIVE")) {
        throw "PE ARM64 GC discarded a live associative COMDAT"
    }

    & $linker --target=pe-arm64 -o (Join-Path $scratch "simple.exe") (Join-Path $scratch "simple.obj") $kernel32
    if ($LASTEXITCODE -ne 0) { throw "PE ARM64 no-GC baseline link failed" }
    & (Join-Path $scratch "simple.exe")
    if ($LASTEXITCODE -ne 0) { throw "PE ARM64 no-GC baseline executable returned $LASTEXITCODE" }

    $gcSize = (Get-Item (Join-Path $scratch "gc.exe")).Length
    Write-Output "PASS: PE ARM64 section GC, COMDAT, imports, and no-GC baseline ($gcSize bytes)"
} finally {
    Pop-Location
}