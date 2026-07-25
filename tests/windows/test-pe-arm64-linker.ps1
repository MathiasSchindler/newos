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

    & .\tests\windows\build-windows-freestanding.ps1 -Compiler $Compiler -TargetTriple aarch64-w64-windows-gnu -Tools linker,true,false
    if ($LASTEXITCODE -ne 0) { throw "Failed to build the Windows ARM64 linker" }

    $linker = "build\freestanding-windows-aarch64\linker.exe"
    $minimalTrue = "build\freestanding-windows-aarch64\true.exe"
    $minimalFalse = "build\freestanding-windows-aarch64\false.exe"
    $scratch = "tests\tmp\windows-pe-arm64-linker"
    New-Item -ItemType Directory -Force $scratch | Out-Null

    & $minimalTrue
    if ($LASTEXITCODE -ne 0) { throw "Minimal true.exe returned $LASTEXITCODE" }
    & $minimalFalse
    if ($LASTEXITCODE -ne 1) { throw "Minimal false.exe returned $LASTEXITCODE" }
    if ((Get-Item $minimalTrue).Length -ne 1024 -or (Get-Item $minimalFalse).Length -ne 1024) {
        throw "Minimal true.exe and false.exe must each be 1024 bytes"
    }

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
    [IO.File]::WriteAllText((Join-Path $scratch "writable.c"), @'
__declspec(dllimport) void ExitProcess(unsigned int exit_code);

static volatile unsigned int value = 41U;
static volatile unsigned char scratch[4096];

void mainCRTStartup(void) {
    scratch[0] = 1U;
    value += scratch[0];
    ExitProcess(value == 42U ? 0U : 1U);
}
'@)
    [IO.File]::WriteAllText((Join-Path $scratch "stack.c"), @'
__declspec(dllimport) void ExitProcess(unsigned int exit_code);

__attribute__((noinline))
static unsigned int use_large_stack(unsigned int seed) {
    volatile unsigned char scratch[8192];
    scratch[0] = (unsigned char)seed;
    scratch[sizeof(scratch) - 1U] = (unsigned char)(seed + 1U);
    return (unsigned int)scratch[0] + (unsigned int)scratch[sizeof(scratch) - 1U];
}

void mainCRTStartup(void) {
    ExitProcess(use_large_stack(20U) == 41U ? 0U : 1U);
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
    foreach ($name in @("entry", "first", "second", "simple", "writable", "stack")) {
        & $Compiler @commonFlags -c (Join-Path $scratch "$name.c") -o (Join-Path $scratch "$name.obj")
        if ($LASTEXITCODE -ne 0) { throw "Failed to compile $name.c" }
    }
    & $Compiler @commonFlags -c "src\platform\windows\minimal_start.c" -o (Join-Path $scratch "minimal_start.obj")
    if ($LASTEXITCODE -ne 0) { throw "Failed to compile the minimal Windows startup" }
    foreach ($name in @("true", "false")) {
        & $Compiler @commonFlags -Isrc/shared -c "src\tools\$name.c" -o (Join-Path $scratch "$name.obj")
        if ($LASTEXITCODE -ne 0) { throw "Failed to compile $name.c" }
    }
    & $Compiler --target=aarch64-w64-windows-gnu -c "src\arch\aarch64\windows\chkstk.S" -o (Join-Path $scratch "chkstk.obj")
    if ($LASTEXITCODE -ne 0) { throw "Failed to compile the ARM64 Windows stack probe" }
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

    $simpleLayout = & $readobj --file-headers --sections --coff-imports (Join-Path $scratch "simple.exe") | Out-String
    if ((Get-Item (Join-Path $scratch "simple.exe")).Length -ne 1024 -or
        $simpleLayout -notmatch "SectionCount: 1" -or $simpleLayout -match "Name: \.idata") {
        throw "PE ARM64 compact import layout is not a one-section 1024-byte image"
    }

    foreach ($name in @("true", "false")) {
        $minimalOutput = Join-Path $scratch "$name-own.exe"
        & $linker --target=pe-arm64 --gc-sections -o $minimalOutput `
            (Join-Path $scratch "minimal_start.obj") (Join-Path $scratch "$name.obj") $kernel32
        if ($LASTEXITCODE -ne 0) { throw "PE ARM64 minimal $name link failed" }
        & $minimalOutput
        $expectedExitCode = if ($name -eq "true") { 0 } else { 1 }
        if ($LASTEXITCODE -ne $expectedExitCode -or (Get-Item $minimalOutput).Length -ne 1024) {
            throw "PE ARM64 minimal $name executable is not a working 1024-byte image"
        }
    }

    & $linker --target=pe-arm64 --gc-sections -o (Join-Path $scratch "writable.exe") `
        (Join-Path $scratch "writable.obj") $kernel32
    if ($LASTEXITCODE -ne 0) { throw "PE ARM64 writable-data link failed" }
    & (Join-Path $scratch "writable.exe")
    if ($LASTEXITCODE -ne 0) { throw "PE ARM64 writable-data executable returned $LASTEXITCODE" }
    $writableLayout = & $readobj --file-headers --sections --coff-imports (Join-Path $scratch "writable.exe") | Out-String
    if ($writableLayout -notmatch "SectionCount: 2" -or $writableLayout -notmatch "Name: \.data" -or
        $writableLayout -match "Name: \.idata" -or $writableLayout -notmatch "IMAGE_SCN_MEM_WRITE") {
        throw "PE ARM64 writable data is not separated from compact RX imports"
    }

    & $linker --target=pe-arm64 --gc-sections -o (Join-Path $scratch "stack-own.exe") `
        (Join-Path $scratch "stack.obj") (Join-Path $scratch "chkstk.obj") $kernel32
    if ($LASTEXITCODE -ne 0) { throw "PE ARM64 stack-probe link failed" }
    & (Join-Path $scratch "stack-own.exe")
    if ($LASTEXITCODE -ne 0) { throw "PE ARM64 stack-probe executable returned $LASTEXITCODE" }

    $importLibraryDir = "build\freestanding-windows-aarch64\.imports"
    $lldFlags = @(
        "--target=aarch64-w64-windows-gnu", "-nostdlib", "-fuse-ld=lld",
        "-Wl,-e,mainCRTStartup", "-Wl,-s", "-Wl,--gc-sections",
        "-Wl,--no-insert-timestamp", "-Wl,/merge:.rdata=.text",
        "-L$importLibraryDir", "-lkernel32"
    )
    & $Compiler (Join-Path $scratch "simple.obj") (Join-Path $scratch "chkstk.obj") @lldFlags -o (Join-Path $scratch "simple-lld.exe")
    if ($LASTEXITCODE -ne 0 -or (Get-Item (Join-Path $scratch "simple-lld.exe")).Length -ne 1024) {
        throw "LLD did not discard the unreferenced COMDAT stack probe"
    }
    & $Compiler (Join-Path $scratch "stack.obj") (Join-Path $scratch "chkstk.obj") @lldFlags -o (Join-Path $scratch "stack-lld.exe")
    if ($LASTEXITCODE -ne 0) { throw "LLD stack-probe link failed" }
    & (Join-Path $scratch "stack-lld.exe")
    if ($LASTEXITCODE -ne 0) { throw "LLD stack-probe executable returned $LASTEXITCODE" }

    $gcSize = (Get-Item (Join-Path $scratch "gc.exe")).Length
    Write-Output "PASS: PE ARM64 compact layout, section GC, COMDAT, imports, stack probes, and writable data ($gcSize bytes)"
} finally {
    Pop-Location
}
