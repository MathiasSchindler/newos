param(
    [string]$Compiler = 'clang',
    [string]$BuildDir = 'tests/tmp/windows-arm64-stack-probe'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Push-Location $repoRoot
try {
    $compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
    $compilerDirectory = Split-Path -Parent $compilerPath
    $dllTool = Join-Path $compilerDirectory 'llvm-dlltool.exe'
    $readobj = Join-Path $compilerDirectory 'llvm-readobj.exe'
    New-Item -ItemType Directory -Force $BuildDir | Out-Null
    & $dllTool -m arm64 -d src/platform/windows/imports/kernel32.def -l "$BuildDir/libkernel32.a"
    if ($LASTEXITCODE -ne 0) { throw 'Could not build Kernel32 imports' }
    $target = '--target=aarch64-w64-windows-gnu'
    & $compilerPath $target -c src/arch/aarch64/windows/chkstk.S -o "$BuildDir/chkstk.obj"
    if ($LASTEXITCODE -ne 0) { throw 'Could not compile the stack probe' }
    & $compilerPath $target -c tests/fixtures/windows_arm64_stack_probe.S -o "$BuildDir/abi.obj"
    if ($LASTEXITCODE -ne 0) { throw 'Could not compile the ABI fixture' }
    & $compilerPath $target -DSTACK_PROBE_NOOP_TEST -c tests/fixtures/windows_arm64_stack_probe.S -o "$BuildDir/noop.obj"
    if ($LASTEXITCODE -ne 0) { throw 'Could not compile the negative control' }
    $flags = @($target, '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
        '-Oz', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
        '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-ffunction-sections', '-fdata-sections')
    $links = @('-nostdlib', '-fuse-ld=lld', '-Wl,-e,mainCRTStartup', '-Wl,--gc-sections',
        '-Xlinker', '--stack', '-Xlinker', '1048576,4096', "-L$BuildDir", '-lkernel32')
    & $compilerPath @flags -c tests/fixtures/windows_arm64_stack_probe.c -o "$BuildDir/fixture.obj"
    if ($LASTEXITCODE -ne 0) { throw 'Could not compile the C fixture' }
    $symbols = & $readobj --symbols "$BuildDir/fixture.obj" | Out-String
    if ($LASTEXITCODE -ne 0 -or $symbols -notmatch 'Name: __chkstk') { throw 'Clang did not emit a stack-probe reference' }
    foreach ($mode in @('object', 'lto')) {
        $inputs = @("$BuildDir/fixture.obj")
        if ($mode -eq 'lto') { $inputs = @('-flto', 'tests/fixtures/windows_arm64_stack_probe.c') }
        $executable = "$BuildDir/probe-$mode.exe"
        & $compilerPath @flags @inputs "$BuildDir/abi.obj" "$BuildDir/chkstk.obj" @links -o $executable
        if ($LASTEXITCODE -ne 0) { throw "Could not link $mode stack-probe test" }
        & $executable
        if ($LASTEXITCODE -ne 0) { throw "$mode stack-probe test failed with exit $LASTEXITCODE" }
        $imports = & $readobj --coff-imports $executable | Out-String
        if ($LASTEXITCODE -ne 0 -or $imports -notmatch 'Name: KERNEL32.dll' -or
            ([regex]::Matches($imports, 'Name: .*\.dll', 'IgnoreCase')).Count -ne 1) {
            throw "$mode executable is not Kernel32-only"
        }
        Write-Output "PASS ${mode}: ABI preservation, page boundaries, 128 KiB guard-page growth, compiler-generated frame, Kernel32-only"
    }
    & $compilerPath "$BuildDir/fixture.obj" "$BuildDir/noop.obj" $target @links -o "$BuildDir/negative.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Could not link the negative control' }
    & "$BuildDir/negative.exe"
    if ($LASTEXITCODE -ne 30) { throw "No-op control exited $LASTEXITCODE; expected unprobed-page failure 30" }
    Write-Output 'PASS no-op negative control rejected: stack pages were not committed'
} finally {
    Pop-Location
}