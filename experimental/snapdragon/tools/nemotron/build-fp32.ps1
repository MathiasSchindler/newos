param(
    [string]$Compiler = 'clang',
    [string]$SdkRoot = 'D:\newos-data\nemotron-asr\m1\sdk\full',
    [string]$SdkArchive = 'experimental\snapdragon\data\v2.50.0.260828.zip',
    [switch]$Sign
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Push-Location $repo
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
try {
    $build = Join-Path $repo 'experimental\snapdragon\build\nemotron-fp32'
    $source = 'experimental\snapdragon\src\apps\nemotron\fp32'
    $include = Join-Path $SdkRoot 'include\QNN'
    $hex = Join-Path $repo 'experimental\snapdragon\data\hexagon-toolchain\6.6.0.0\tools\HEXAGON_Tools\19.0.07\Tools\bin\hexagon-clang.exe'
    $ld = Join-Path $repo 'experimental\snapdragon\build\hexagon-llvm\bin\lld.exe'
    $cc = (Get-Command $Compiler -ErrorAction Stop).Source
    $bin = Split-Path $cc
    foreach ($path in @($hex, $ld)) {
        if (-not (Test-Path -LiteralPath $path)) { throw "Missing local SDK/tool: $path" }
    }
    $archive = (Resolve-Path -LiteralPath $SdkArchive).Path
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine
        'A346EA0E2C8631B46D57261A4969994CD9CC34124A8355BBC7B08B2C8BD859A5') {
        throw 'SDK archive fingerprint differs from the canonical runtime SDK'
    }
    if ([IO.Path]::GetFullPath($SdkRoot).StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Proprietary SDK headers must remain outside the repository'
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($entry in $zip.Entries) {
            if ($entry.Length -gt 0 -and $entry.FullName -match '^qairt/2\.50\.0\.260828/include/QNN/(.+\.h)$') {
                $relative = $Matches[1].Replace('/', '\')
                if ($relative.Split('\') -contains '..') { throw 'SDK traversal path rejected' }
                $destination = Join-Path $include $relative
                New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $true)
            }
        }
    } finally { $zip.Dispose() }
    New-Item -ItemType Directory -Force -Path "$build\compiler-scratch" | Out-Null
    $env:TEMP = "$build\compiler-scratch"
    $env:TMP = $env:TEMP
    $common = @('-Oz', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
        '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-ffunction-sections',
        '-fdata-sections', '-Wall', '-Wextra', '-Werror', '-std=c11', "-I$include")
    & $cc --target=aarch64-w64-windows-gnu @common -c "$source\nm_package.c" -o "$build\nm_package.obj"
    if ($LASTEXITCODE) { throw 'Host package compilation failed' }
    & "$bin\llvm-dlltool.exe" -m arm64 -d 'experimental\snapdragon\src\apps\nemotron\imports\kernel32.def' -l "$build\libkernel32.a"
    if ($LASTEXITCODE) { throw 'Host import library failed' }
    & $cc --target=aarch64-w64-windows-gnu @common -nostdlib -fuse-ld=lld `
        '-Wl,-e,mainCRTStartup' '-Wl,--gc-sections' '-Wl,--no-insert-timestamp' `
        "$source\nm_hexagon_test.c" "$source\nm_hexagon.c" "-L$build" -lkernel32 `
        -o "$build\test-fp32-helpers.exe"
    if ($LASTEXITCODE) { throw 'Compiler-helper test build failed' }
    & "$build\test-fp32-helpers.exe"
    if ($LASTEXITCODE) { throw 'Compiler-helper tests failed' }
    # Only the package DLL uses the SDK engine ABI. No host execution implementation is linked.
    & $cc --target=aarch64-w64-windows-gnu -shared -nostdlib -fuse-ld=lld `
        '-Wl,-e,NmFp32DllEntry' '-Wl,--gc-sections' '-Wl,--no-insert-timestamp' `
        "$build\nm_package.obj" "-L$build" -lkernel32 -o "$build\NmFp32Host.dll"
    if ($LASTEXITCODE) { throw 'Host package link failed' }
    foreach ($unit in @('nm_package', 'nm_fp32_kernels', 'nm_hexagon')) {
        & $hex -mv73 -G0 -mhvx -mhvx-length=128B -DNM_FP32_HVX=1 -fPIC @common -O2 -c "$source\$unit.c" -o "$build\$unit.o"
        if ($LASTEXITCODE) { throw "V73 compilation failed: $unit" }
    }
    # QHPI resolves against the already loaded signed QNN skel, not a second engine ELF.
    & $ld -flavor gnu -shared --hash-style=sysv --no-rosegment `
        -z max-page-size=4096 -z separate-loadable-segments -z norelro `
        -soname libNmFp32V73.so "$build\nm_package.o" "$build\nm_fp32_kernels.o" "$build\nm_hexagon.o" `
        -o "$build\libNmFp32V73.so"
    if ($LASTEXITCODE) { throw 'V73 package link failed' }
    $hostAudit = & "$bin\llvm-readobj.exe" --file-headers --coff-imports --coff-exports "$build\NmFp32Host.dll" | Out-String
    if ($LASTEXITCODE) { throw 'Host PE inspection failed' }
    $imports = @([regex]::Matches($hostAudit, '(?m)^\s*Name: (.+\.dll)\s*$') |
        ForEach-Object { $_.Groups[1].Value.Trim() })
    if ($hostAudit -notmatch 'IMAGE_FILE_MACHINE_ARM64' -or
        $imports.Count -ne 1 -or $imports[0] -ine 'KERNEL32.dll' -or
        $hostAudit -notmatch 'ExceptionTableRVA: 0x0\b' -or
        $hostAudit -notmatch 'Name: NmFp32InterfaceProvider' -or $hostAudit -notmatch 'Name: qhpi_init') {
        throw "Host no-CRT/import/export audit failed`n$hostAudit"
    }
    $testAudit = & "$bin\llvm-readobj.exe" --file-headers --coff-imports "$build\test-fp32-helpers.exe" | Out-String
    $testImports = @([regex]::Matches($testAudit, '(?m)^\s*Name: (.+\.dll)\s*$') |
        ForEach-Object { $_.Groups[1].Value.Trim() })
    if ($LASTEXITCODE -or $testAudit -notmatch 'IMAGE_FILE_MACHINE_ARM64' -or
        $testImports.Count -ne 1 -or $testImports[0] -ine 'KERNEL32.dll' -or
        $testAudit -notmatch 'ExceptionTableRVA: 0x0\b' -or $testAudit -notmatch 'CLRRuntimeHeaderRVA: 0x0\b') {
        throw "Compiler-helper test no-CRT audit failed`n$testAudit"
    }
    $dynamic = & "$bin\llvm-readelf.exe" --dynamic "$build\libNmFp32V73.so" | Out-String
    $symbols = & "$bin\llvm-readelf.exe" --dyn-syms "$build\libNmFp32V73.so" | Out-String
    $needed = @([regex]::Matches($dynamic, 'Shared library: \[([^\]]+)\]') |
        ForEach-Object { $_.Groups[1].Value })
    $undefined = @([regex]::Matches($symbols, '(?m)^\s*\d+:.*\bUND\s+(\S+)\s*$') |
        ForEach-Object { $_.Groups[1].Value })
    $allowed = @('qhpi_register_ops_v1', 'qhpi_tensor_shape', 'qhpi_tensor_type',
        'qhpi_tensor_raw_data', 'qhpi_num_slices', 'qhpi_slice_number')
    if ($needed.Count -ne 0 -or
        @($undefined | Where-Object { $_ -notin $allowed }).Count -or
        $symbols -notmatch '\bNmFp32InterfaceProvider\b' -or $symbols -notmatch '\bqhpi_init\b') {
        throw "DSP engine-only/no-CRT audit failed`n$dynamic`n$symbols"
    }
    [IO.File]::WriteAllText("$build\host-audit.txt", $hostAudit)
    [IO.File]::WriteAllText("$build\dsp-audit.txt", $dynamic + $symbols)
    Write-Output "ARM64 prepare-only DLL and V73 DSP module built; no CRT, six audited QHPI engine imports"
    if ($Sign) { & "$PSScriptRoot\sign-fp32.ps1" }
} finally {
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
    Pop-Location
}
