param(
    [string]$Compiler = 'clang',
    [switch]$Sign,
    [switch]$HvxProfile,
    [string]$StageDirectory = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Push-Location $repo
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
try {
    $build = Join-Path $repo 'experimental\snapdragon\build\nemotron-fastrpc-fp32'
    $src = 'experimental\snapdragon\src\apps\nemotron'
    $sdk = Join-Path $repo 'experimental\snapdragon\data\hexagon-toolchain\6.6.0.0'
    $hex = Join-Path $sdk 'tools\HEXAGON_Tools\19.0.07\Tools\bin\hexagon-clang.exe'
    $ld = Join-Path $repo 'experimental\snapdragon\build\hexagon-llvm\bin\lld.exe'
    $cc = (Get-Command $Compiler -ErrorAction Stop).Source
    $bin = Split-Path $cc
    if ($Sign -and -not $StageDirectory) {
        $StageDirectory = Join-Path 'D:\newos-data\nemotron-asr\m1\fastrpc-fp32-profiles' `
            ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    }
    if ($Sign -and (Test-Path -LiteralPath $StageDirectory) -and
        @(Get-ChildItem -LiteralPath $StageDirectory -Force).Count) { throw 'Signing requires a new, empty snapshot directory' }
    $sourceReceipt = [ordered]@{}
    foreach ($name in @('nm_fp32_kernels.c', 'nm_fp32.h', 'nm_fastrpc_skel.c', 'protocol.h',
                       'nm_fastrpc_host.c', 'nm_fastrpc.h', 'nm_hvx_profile.h', 'nm_hexagon.c', 'nm_dsp_memory.c')) {
        $file = (Resolve-Path "$src\fp32\$name").Path
        $sourceReceipt[$name] = @{file=$file;sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    $native = (Resolve-Path "$src\nemotron.c").Path
    $sourceReceipt['nemotron.c'] = @{file=$native;sha256=(Get-FileHash -LiteralPath $native -Algorithm SHA256).Hash.ToLowerInvariant()}
    foreach ($file in @("$src\nemotron_reader.c", "$src\nemotron_reader.h", "$src\nemotron_windows.c",
        "$src\nemotron_windows.h", "$src\nemotron_wav.h", "$src\nemotron_tokenizer.c", "$src\nemotron_tokenizer.h",
        "$src\nemotron_format.h", "$src\imports\kernel32.def",
        'experimental\snapdragon\src\shared\qnn_abi.h', 'src\shared\crypto\sha256.c', 'src\shared\crypto\sha256.h',
        'src\shared\runtime\memory.c', 'src\shared\runtime\string.c', 'src\shared\runtime\parse.c',
        'src\shared\runtime\unicode_utf8.c',
        'src\platform\windows\thread.c', 'src\arch\aarch64\windows\chkstk.S', 'src\shared\runtime.h')) {
        $full = (Resolve-Path $file).Path
        $sourceReceipt[$file] = @{file=$full;sha256=(Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    New-Item -ItemType Directory -Force -Path "$build\compiler-scratch" | Out-Null
    $env:TEMP = "$build\compiler-scratch"; $env:TMP = $env:TEMP
    $flags = @('-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin',
        '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
        '-ffunction-sections', '-fdata-sections')
    foreach ($unit in @('nm_fastrpc_skel', 'nm_fp32_kernels', 'nm_hexagon', 'nm_dsp_memory')) {
        $instrument = @()
        if ($HvxProfile -and $unit -eq 'nm_fp32_kernels') { $instrument = @('-include', "$src\fp32\nm_hvx_profile.h") }
        if ($HvxProfile -and $unit -eq 'nm_fastrpc_skel') { $instrument = @('-DNM_RPC_HVX_COUNTER_ENABLED=1') }
        & $hex -mv73 -G0 -mhvx -mhvx-length=128B '-DNM_FP32_HVX=1' -fPIC @flags `
            "-I$sdk\rtos\qurt\computev73\include\qurt" "-I$sdk\incs" "-I$sdk\incs\stddef" `
            @instrument -c "$src\fp32\$unit.c" -o "$build\$unit.o"
        if ($LASTEXITCODE) { throw "V73 compilation failed: $unit" }
    }
    & $ld -flavor gnu -shared --hash-style=sysv --no-rosegment -z max-page-size=4096 `
        -z separate-loadable-segments -z norelro -soname nm_fastrpc_skel.so `
        "$build\nm_fastrpc_skel.o" "$build\nm_fp32_kernels.o" "$build\nm_hexagon.o" "$build\nm_dsp_memory.o" `
        -o "$build\nm_fastrpc_skel.so"
    if ($LASTEXITCODE) { throw 'Pure FastRPC V73 link failed' }
    $dynamic = & "$bin\llvm-readelf.exe" --dynamic "$build\nm_fastrpc_skel.so" | Out-String
    $symbols = & "$bin\llvm-readelf.exe" --dyn-syms "$build\nm_fastrpc_skel.so" | Out-String
    $undefined = @([regex]::Matches($symbols, '(?m)^\s*\d+:.*\bUND\s+(\S+)\s*$') |
        ForEach-Object { $_.Groups[1].Value })
    if ($dynamic -match '\(NEEDED\)' -or $symbols -match '\bqhpi_' -or
        @($undefined | Where-Object { $_ -notin @('qurt_hvx_lock', 'qurt_hvx_unlock', 'qurt_sysclock_get_hw_ticks', 'qurt_get_core_pcycles',
            'qurt_mem_region_query', 'qurt_mem_region_attr_get', 'HAP_power_set', 'HAP_power_destroy_client',
            'qurt_thread_create', 'qurt_thread_join', 'qurt_thread_exit', 'qurt_sem_init_val',
            'qurt_sem_destroy', 'qurt_sem_down', 'qurt_sem_add') }).Count -or
        $symbols -notmatch '\bnm_fastrpc_skel_invoke\b') {
        throw "DSP no-QNN/no-libc dependency audit failed`n$dynamic`n$symbols"
    }
    & "$bin\llvm-dlltool.exe" -m arm64 -d "$src\imports\kernel32.def" -l "$build\libkernel32.a"
    if ($LASTEXITCODE) { throw 'Import library failed' }
    & $cc --target=aarch64-w64-windows-gnu -c 'src\arch\aarch64\windows\chkstk.S' -o "$build\chkstk.obj"
    if ($LASTEXITCODE) { throw 'Stack helper failed' }
    & $cc --target=aarch64-w64-windows-gnu @flags -Oz -flto -march=armv8-a+crypto `
        '-DNEWOS_CRYPTO_SHA256_ENABLE_ARM_SHA=1' '-DNEWOS_RUNTIME_ALLOC_LOCK=0' '-DNM_FASTRPC=1' `
        -Isrc\shared -Iexperimental\snapdragon\src\shared "-I$src" -nostdlib -fuse-ld=lld `
        '-Wl,-e,mainCRTStartup' '-Wl,--gc-sections' '-Wl,--icf=safe' '-Wl,--no-insert-timestamp' `
        '-Wl,--stack,1048576' "$src\nemotron.c" "$src\nemotron_reader.c" "$src\nemotron_windows.c" `
        "$src\fp32\nm_fastrpc_host.c" "$src\nemotron_tokenizer.c" 'src\shared\runtime\memory.c' 'src\shared\runtime\string.c' `
        'src\shared\runtime\parse.c' 'src\shared\runtime\unicode_utf8.c' 'src\shared\crypto\sha256.c' 'src\platform\windows\thread.c' `
        "$build\chkstk.obj" "-L$build" -lkernel32 -o "$build\nemotron-fastrpc-fp32.exe"
    if ($LASTEXITCODE) { throw 'No-CRT FastRPC host build failed' }
    $audit = & "$bin\llvm-readobj.exe" --file-headers --coff-imports "$build\nemotron-fastrpc-fp32.exe" | Out-String
    $imports = @([regex]::Matches($audit, '(?m)^\s*Name: (.+\.dll)\s*$') |
        ForEach-Object { $_.Groups[1].Value.Trim() })
    if ($imports.Count -ne 1 -or $imports[0] -ine 'KERNEL32.dll' -or
        $audit -notmatch 'IMAGE_FILE_MACHINE_ARM64' -or $audit -notmatch 'ExceptionTableRVA: 0x0\b' -or
        $audit -notmatch 'CLRRuntimeHeaderRVA: 0x0\b') { throw "Host dependency audit failed`n$audit" }
    [IO.File]::WriteAllText("$build\host-audit.txt", $audit)
    [IO.File]::WriteAllText("$build\dsp-audit.txt", $dynamic + $symbols)
    foreach ($entry in $sourceReceipt.Values) {
        if ((Get-FileHash -LiteralPath $entry.file -Algorithm SHA256).Hash -ine $entry.sha256) {
            throw "Concurrent source edit rejected; do not sign this build: $($entry.file)"
        }
    }
    @{sources=$sourceReceipt;hvxProfile=[bool]$HvxProfile} | ConvertTo-Json -Depth 5 |
        Set-Content -Encoding ASCII -LiteralPath "$build\source-receipt.json"
    Write-Output 'Pure OEM FastRPC FP32 executor built: ARM64 Kernel32-only host, V73 HVX DSP, no QNN/QHPI/libc imports'
    if ($Sign) { & "$PSScriptRoot\sign-fastrpc-fp32.ps1" -StageDirectory $StageDirectory }
} finally { $env:TEMP = $oldTemp; $env:TMP = $oldTmp; Pop-Location }
