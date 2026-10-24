param([string]$Compiler = 'clang')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Push-Location $repo
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
try {
    $build = Join-Path $repo 'experimental\snapdragon\build\nemotron-fp32\loaders'
    $evidence = 'D:\newos-data\nemotron-asr\m1\fp32-package'
    $sdk = 'D:\newos-data\nemotron-asr\m1\sdk\full'
    $archive = (Resolve-Path 'experimental\snapdragon\data\v2.50.0.260828.zip').Path
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine
        'A346EA0E2C8631B46D57261A4969994CD9CC34124A8355BBC7B08B2C8BD859A5') { throw 'SDK fingerprint mismatch' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($entry in $zip.Entries) {
            if ($entry.FullName -match '^qairt/2\.50\.0\.260828/(lib/x86_64-windows-msvc/[^/]+\.dll)$') {
                $path = Join-Path $sdk $Matches[1].Replace('/', '\')
                New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $path, $true)
            }
        }
    } finally { $zip.Dispose() }
    New-Item -ItemType Directory -Force -Path "$build\compiler-scratch" | Out-Null
    $env:TEMP = "$build\compiler-scratch"
    $env:TMP = $env:TEMP
    $cc = (Get-Command $Compiler).Source
    $bin = Split-Path $cc
    $llvmRoot = Split-Path $bin
    $src = 'experimental\snapdragon\src\apps\nemotron'
    $common = @('-std=c11', '-Oz', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
        '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
        '-ffunction-sections', '-fdata-sections', '-Isrc\shared', '-Iexperimental\snapdragon\src\shared', "-I$src")
    foreach ($arch in @('aarch64', 'x86_64')) {
        $directory = Join-Path $build $arch
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        $triple = "$arch-w64-windows-gnu"
        $machine = if ($arch -eq 'aarch64') { 'arm64' } else { 'i386:x86-64' }
        & "$bin\llvm-dlltool.exe" -m $machine -d "$src\imports\kernel32.def" -l "$directory\libkernel32.a"
        if ($LASTEXITCODE) { throw 'Probe import library failed' }
        & $cc "--target=$triple" -c "src\arch\$arch\windows\chkstk.S" -o "$directory\chkstk.obj"
        if ($LASTEXITCODE) { throw 'Probe stack helper failed' }
        & $cc "--target=$triple" @common -nostdlib -fuse-ld=lld '-Wl,-e,mainCRTStartup' '-Wl,--gc-sections' `
            "$src\fp32\nm_package_probe.c" "$src\nemotron_windows.c" "$src\nemotron_reader.c" `
            'src\shared\runtime\memory.c' 'src\shared\runtime\string.c' 'src\shared\runtime\parse.c' `
            'src\shared\crypto\sha256.c' 'src\platform\windows\thread.c' "$directory\chkstk.obj" `
            "-L$directory" -lkernel32 -o "$directory\package-loader-probe.exe"
        if ($LASTEXITCODE) { throw "$arch probe build failed" }
        & $cc "--target=$triple" @common "-I$sdk\include\QNN" -c "$src\fp32\nm_package.c" -o "$directory\nm_package.obj"
        if ($LASTEXITCODE) { throw "$arch host package build failed" }
        & $cc "--target=$triple" -shared -nostdlib -fuse-ld=lld '-Wl,-e,NmFp32DllEntry' `
            "$directory\nm_package.obj" "-L$directory" -lkernel32 -o "$directory\NmFp32Host.dll"
        if ($LASTEXITCODE) { throw "$arch host package link failed" }
        $audit = & "$bin\llvm-readobj.exe" --coff-imports "$directory\package-loader-probe.exe" | Out-String
        $imports = @([regex]::Matches($audit, '(?m)^\s*Name: (.+\.dll)\s*$') | ForEach-Object { $_.Groups[1].Value.Trim() })
        if ($imports.Count -ne 1 -or $imports[0] -ine 'KERNEL32.dll') { throw "$arch probe no-CRT audit failed" }
        $backend = if ($arch -eq 'aarch64') { Join-Path $repo 'experimental\snapdragon\build\nemotron-asr\QnnHtp.dll' } else { Join-Path $sdk 'lib\x86_64-windows-msvc\QnnHtp.dll' }
        $env:NM_FP32_LOADER_BACKEND = $backend
        $env:NM_FP32_LOADER_BACKEND_SHA256 = (Get-FileHash -LiteralPath $backend -Algorithm SHA256).Hash
        $env:NM_FP32_HOST_PACKAGE = "$directory\NmFp32Host.dll"
        $env:NM_FP32_HOST_SHA256 = (Get-FileHash -LiteralPath $env:NM_FP32_HOST_PACKAGE -Algorithm SHA256).Hash
        foreach ($target in @('CPU', 'DEFAULT')) {
            $log = Join-Path $evidence "windows-$arch-$target-loader.log"
            $ErrorActionPreference = 'Continue'
            & "$directory\package-loader-probe.exe" $target 2>&1 | Out-File -Encoding utf8 -LiteralPath $log
            $exit = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            Write-Output "$arch target=$target exit=$exit evidence=$log"
            Select-String -LiteralPath $log -Pattern 'status=|LoadLibrary|not implemented|provider present=' | ForEach-Object { $_.Line }
        }
        & $cc "--target=$arch-pc-windows-msvc" -std=c++17 -O2 -fno-exceptions -fno-rtti -nostdinc++ `
            "-I$llvmRoot\include\c++\v1" "-I$llvmRoot\$arch-w64-mingw32\include" "-I$sdk\include\QNN" `
            -c "$src\fp32\nm_legacy_loader_probe.cpp" -o "$directory\nm_legacy_loader_probe.obj"
        if ($LASTEXITCODE) { throw "$arch documented legacy SDK callback compilation failed" }
        & $cc "--target=$triple" @common '-DNM_FP32_LEGACY_LOADER_PROBE=1' "-I$sdk\include\QNN" `
            -c "$src\fp32\nm_package.c" -o "$directory\nm_package_legacy.obj"
        if ($LASTEXITCODE) { throw "$arch legacy provider compilation failed" }
        & $cc "--target=$triple" -shared -nostdlib -fuse-ld=lld '-Wl,-e,NmFp32DllEntry' `
            "$directory\nm_package_legacy.obj" "$directory\nm_legacy_loader_probe.obj" `
            "-L$directory" -lkernel32 -o "$directory\NmFp32LegacyLoaderProbe.dll"
        if ($LASTEXITCODE) { throw "$arch MSVC-ABI legacy callback link failed" }
        $env:NM_FP32_HOST_PACKAGE = "$directory\NmFp32LegacyLoaderProbe.dll"
        $env:NM_FP32_HOST_SHA256 = (Get-FileHash -LiteralPath $env:NM_FP32_HOST_PACKAGE -Algorithm SHA256).Hash
        $log = Join-Path $evidence "windows-$arch-legacy-CPU-loader.log"
        $ErrorActionPreference = 'Continue'
        & "$directory\package-loader-probe.exe" CPU 2>&1 | Out-File -Encoding utf8 -LiteralPath $log
        $exit = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        Write-Output "$arch MSVC-ABI legacy target=CPU exit=$exit evidence=$log"
        Select-String -LiteralPath $log -Pattern 'status=|not implemented|entry present=|entry reached=' | ForEach-Object { $_.Line }
    }
} finally {
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
    Pop-Location
}
