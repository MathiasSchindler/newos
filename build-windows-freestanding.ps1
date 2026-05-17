param(
    [string]$Compiler = "clang",
    [string]$TargetTriple = "x86_64-w64-windows-gnu",
    [string]$BuildDir = "build/freestanding-windows-x86_64",
    [string]$MsysRoot = "C:\msys64",
    [string[]]$Tools = @(),
    [switch]$Clean,
    [switch]$VerboseCommands
)

$ErrorActionPreference = "Stop"

function Find-CommandPath([string]$Name) {
    if ([System.IO.Path]::IsPathRooted($Name) -or $Name.Contains("\") -or $Name.Contains("/")) {
        if (Test-Path $Name) { return (Resolve-Path $Name).Path }
        return $null
    }

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    return $null
}

function Find-Clang([string]$Name, [string]$Root) {
    $path = Find-CommandPath $Name
    if ($path) { return $path }

    $candidates = @(
        "C:\Program Files\LLVM\bin\clang.exe",
        (Join-Path $Root "ucrt64\bin\clang.exe"),
        (Join-Path $Root "clang64\bin\clang.exe"),
        (Join-Path $Root "mingw64\bin\clang.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
    }

    return $null
}

function Split-Words([string]$Text) {
    if ([string]::IsNullOrWhiteSpace($Text)) { return @() }
    return @($Text.Trim() -split '\s+' | Where-Object { $_ -ne "" })
}

function Read-MakeVariable([string]$MakefileText, [string]$Name) {
    $escaped = [regex]::Escape($Name)
    $match = [regex]::Match($MakefileText, "(?m)^$escaped\s*(?:\?|:)?=\s*(.+)$")
    if (-not $match.Success) { throw "Could not find Makefile variable $Name" }
    return Split-Words $match.Groups[1].Value
}

function Read-ManifestSources([string]$ManifestText, [string]$MacroName) {
    $escaped = [regex]::Escape($MacroName)
    $match = [regex]::Match($ManifestText, "(?s)#define\s+$escaped\(X\)\s*\\\s*(.*?)(?=\r?\n\r?\n/\*|\r?\n#define|\r?\n#endif)")
    if (-not $match.Success) { throw "Could not find manifest macro $MacroName" }

    $sources = New-Object System.Collections.Generic.List[string]
    foreach ($sourceMatch in [regex]::Matches($match.Groups[1].Value, 'X\("([^"]+)"\)')) {
        $sources.Add($sourceMatch.Groups[1].Value)
    }
    return @($sources)
}

function Add-Unique([string[]]$Items) {
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $result = New-Object System.Collections.Generic.List[string]
    foreach ($item in $Items) {
        if ([string]::IsNullOrWhiteSpace($item)) { continue }
        if ($seen.Add($item)) { $result.Add($item) }
    }
    return @($result)
}

function Remove-Tools([string[]]$InputTools, [string[]]$RemovedTools) {
    $removed = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
    foreach ($tool in $RemovedTools) { [void]$removed.Add($tool) }
    return @($InputTools | Where-Object { -not $removed.Contains($_) })
}

function Assert-SourceFilesExist([string[]]$Sources) {
    foreach ($source in $Sources) {
        if (-not (Test-Path -LiteralPath $source)) { throw "Missing source file: $source" }
    }
}

function Invoke-CompileTool(
    [string]$CompilerPath,
    [string]$Triple,
    [string]$ToolName,
    [string[]]$Sources,
    [string[]]$ExtraCFlags,
    [string[]]$LinkFlags,
    [string]$OutputPath,
    [switch]$ShowCommand
) {
    Assert-SourceFilesExist $Sources
    $arguments = @("--target=$Triple") + $script:WindowsCFlags + $ExtraCFlags + $Sources + $LinkFlags + @("-o", $OutputPath)
    if ($ShowCommand) {
        Write-Output ("{0} {1}" -f $CompilerPath, ($arguments -join " "))
    } else {
        Write-Output ("[{0}/{1}] {2}" -f $script:BuiltCount, $script:RequestedToolCount, $ToolName)
    }
    & $CompilerPath @arguments
    if ($LASTEXITCODE -ne 0) { throw "clang failed while building $ToolName" }
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repoRoot

$compilerPath = Find-Clang $Compiler $MsysRoot
if (-not $compilerPath) {
    throw "Could not find '$Compiler'. Install LLVM/Clang for Windows or pass -Compiler <path-to-clang.exe>."
}

$compilerDir = Split-Path -Parent $compilerPath
if ($env:PATH -notlike "*$compilerDir*") {
    $env:PATH = $compilerDir + ";" + $env:PATH
}

$makefileText = Get-Content "Makefile" -Raw
$manifestText = Get-Content "src/compiler/source_manifest.h" -Raw

$allTools = Read-MakeVariable $makefileText "TOOLS"
$selectedTools = if ($Tools.Count -gt 0) { $Tools } else { $allTools }
$unknownTools = @($selectedTools | Where-Object { $allTools -notcontains $_ })
if ($unknownTools.Count -gt 0) { throw "Unknown tool(s): $($unknownTools -join ', ')" }

$compilerSources = @(Read-ManifestSources $manifestText "FOREACH_COMPILER_SOURCE")
$sharedSources = @(Read-ManifestSources $manifestText "FOREACH_SHARED_SOURCE")
$imageManifestSources = @(Read-ManifestSources $manifestText "FOREACH_IMAGE_SOURCE")
$cryptoSources = @(Read-ManifestSources $manifestText "FOREACH_CRYPTO_SOURCE")
$tlsSources = @(Read-ManifestSources $manifestText "FOREACH_TLS_SOURCE")
$tuiSources = @(Read-ManifestSources $manifestText "FOREACH_TUI_SOURCE")
$shellSources = @(Read-ManifestSources $manifestText "FOREACH_SHELL_SOURCE")
$sshClientSources = @(Read-ManifestSources $manifestText "FOREACH_SSH_CLIENT_SOURCE")
$sshdToolSources = @(Read-ManifestSources $manifestText "FOREACH_SSHD_SOURCE")

$variables = @{}
$variables["MAKE_TOOL_SOURCES"] = Read-MakeVariable $makefileText "MAKE_TOOL_SOURCES"
$variables["HTTPD_TOOL_SOURCES"] = Read-MakeVariable $makefileText "HTTPD_TOOL_SOURCES"
$variables["SERVICE_TOOL_SOURCES"] = Read-MakeVariable $makefileText "SERVICE_TOOL_SOURCES"
$variables["EDITOR_TOOL_SOURCES"] = Read-MakeVariable $makefileText "EDITOR_TOOL_SOURCES"
$variables["MAIL_TOOL_SOURCES"] = Read-MakeVariable $makefileText "MAIL_TOOL_SOURCES"
$variables["COMPILER_SOURCES"] = $compilerSources
$variables["SHARED_SOURCES"] = $sharedSources
$variables["TLS_SOURCES"] = $tlsSources
$variables["CRYPTO_SOURCES"] = $cryptoSources
$variables["TUI_SOURCES"] = $tuiSources
$variables["SSH_CLIENT_SOURCES"] = $sshClientSources
$variables["SSHD_TOOL_SOURCES"] = $sshdToolSources

$runtimeSources = @(Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_RUNTIME_SOURCES")
$imageSources = Add-Unique (@($imageManifestSources) + @("src/shared/compression/crc32.c", "src/shared/compression/zlib.c"))
$hashSources = @("src/shared/hash_util.c", "src/shared/crypto/md5.c", "src/shared/crypto/sha256.c", "src/shared/crypto/sha512.c")
$archiveSources = @("src/shared/archive_util.c", "src/shared/compression/crc32.c", "src/shared/compression/lzss.c", "src/shared/crypto/sha256.c")
$awkSources = @("src/tools/awk/awk_parse.c", "src/tools/awk/awk_exec.c")
$xmlSources = @("src/shared/xml.c", "src/shared/xml_stream.c", "src/shared/xml_dtd.c", "src/shared/tool_xml.c")
$editorSources = Add-Unique (@($variables["EDITOR_TOOL_SOURCES"]) + @($tuiSources))
$mailSources = Add-Unique (@($variables["MAIL_TOOL_SOURCES"]) + @($tuiSources) + @($tlsSources) + @($cryptoSources) + @("src/platform/windows/tls.c"))
$nccSources = Add-Unique (@($compilerSources) + @($sharedSources))
$shellToolSources = Add-Unique (@($shellSources) + @($sharedSources))
$makeToolSources = Add-Unique (@($variables["MAKE_TOOL_SOURCES"]) + @($sharedSources))
$httpdSources = Add-Unique (@($variables["HTTPD_TOOL_SOURCES"]) + @($sharedSources))
$serviceSources = Add-Unique (@($variables["SERVICE_TOOL_SOURCES"]) + @($sharedSources))
$sshTransportSources = @($sshClientSources | Where-Object { $_ -match 'src/tools/ssh/ssh_(core|client_io)\.c$' })
$sshCryptoSources = Add-Unique (@($cryptoSources) + @("src/shared/crypto/curve25519.c", "src/shared/crypto/ed25519.c", "src/shared/crypto/chacha20_poly1305.c", "src/shared/crypto/ssh_kdf.c"))
$sshSources = Add-Unique (@($sshClientSources) + @($sshCryptoSources) + @($tlsSources) + @("src/platform/windows/tls.c") + @($sharedSources))
$sshdSources = Add-Unique (@($sshdToolSources) + @($sshTransportSources) + @($sshCryptoSources) + @($tlsSources) + @("src/platform/windows/tls.c") + @($sharedSources))

$bignumTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_BIGNUM_TOOLS"
$hashTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_HASH_TOOLS"
$imageTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_IMAGE_TOOLS"
$regexTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_REGEX_TOOLS"
$archiveTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_ARCHIVE_TOOLS"
$awkTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_AWK_TOOLS"
$xmlTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_XML_TOOLS"
$tuiTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_TUI_TOOLS"
$mailTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_MAIL_TOOLS"
$wgetTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_WGET_TOOLS"
$nccTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_NCC_TOOLS"
$shellTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_SHELL_TOOLS"
$makeTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_MAKE_TOOLS"
$httpdTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_HTTPD_TOOLS"
$serviceTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_SERVICE_TOOLS"
$sshTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_SSH_TOOLS"
$sshdTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_SSHD_TOOLS"
$aliasTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_ALIAS_TOOLS"

$specialTools = Add-Unique (@("wtf") + $imageTools + $bignumTools + $hashTools + $regexTools + $archiveTools + $awkTools + $xmlTools + $tuiTools + $mailTools + $wgetTools + $nccTools + $shellTools + $makeTools + $httpdTools + $serviceTools + $sshTools + $sshdTools + $aliasTools)
$genericTools = Remove-Tools $allTools $specialTools

$toolKinds = @{}
foreach ($tool in $genericTools) { $toolKinds[$tool] = "generic" }
foreach ($tool in $imageTools) { $toolKinds[$tool] = "image" }
foreach ($tool in $bignumTools) { $toolKinds[$tool] = "bignum" }
foreach ($tool in $hashTools) { $toolKinds[$tool] = "hash" }
foreach ($tool in $regexTools) { $toolKinds[$tool] = "regex" }
foreach ($tool in $archiveTools) { $toolKinds[$tool] = "archive" }
foreach ($tool in $awkTools) { $toolKinds[$tool] = "awk" }
foreach ($tool in $xmlTools) { $toolKinds[$tool] = "xml" }
foreach ($tool in $tuiTools) { $toolKinds[$tool] = "editor" }
foreach ($tool in $mailTools) { $toolKinds[$tool] = "mail" }
foreach ($tool in $wgetTools) { $toolKinds[$tool] = "wget" }
foreach ($tool in $nccTools) { $toolKinds[$tool] = "ncc" }
foreach ($tool in $shellTools) { $toolKinds[$tool] = "shell" }
foreach ($tool in $makeTools) { $toolKinds[$tool] = "make" }
foreach ($tool in $httpdTools) { $toolKinds[$tool] = "httpd" }
foreach ($tool in $serviceTools) { $toolKinds[$tool] = "service" }
foreach ($tool in $sshTools) { $toolKinds[$tool] = "ssh" }
foreach ($tool in $sshdTools) { $toolKinds[$tool] = "sshd" }
$toolKinds["wtf"] = "wtf"
foreach ($tool in $aliasTools) { $toolKinds[$tool] = "alias" }

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$script:WindowsCFlags = @(
    "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Oz",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-ffunction-sections", "-fdata-sections",
    "-Isrc/shared", "-Isrc/platform/windows"
)
$windowsLdFlags = @("-nostdlib", "-fuse-ld=lld", "-Wl,-e,mainCRTStartup", "-Wl,-s", "-Wl,--gc-sections", "-Wl,--stack,8388608", "-lkernel32", "-lws2_32")
$windowsTlsLdFlags = $windowsLdFlags + @("-lbcrypt")

$script:BuiltCount = 0
$script:RequestedToolCount = $selectedTools.Count
Write-Output "Compiler: $compilerPath"
Write-Output "Target:   $TargetTriple"
Write-Output "Output:   $BuildDir"
Write-Output "Tools:    $($selectedTools.Count)"

foreach ($tool in $selectedTools) {
    if (-not $toolKinds.ContainsKey($tool)) { throw "No Windows freestanding build rule for $tool" }
    $script:BuiltCount += 1
    $output = Join-Path $BuildDir "$tool.exe"
    $kind = $toolKinds[$tool]

    if ($kind -eq "alias") {
        if ($tool -eq "ping6") {
            $sourceAlias = Join-Path $BuildDir "ping.exe"
            if (-not (Test-Path $sourceAlias)) { throw "Cannot create ping6.exe before ping.exe exists" }
            Copy-Item -Force $sourceAlias $output
            Write-Output ("[{0}/{1}] {2}" -f $script:BuiltCount, $script:RequestedToolCount, $tool)
            continue
        }
        throw "No alias rule for $tool"
    }

    $mainSource = "src/tools/$tool.c"
    $sources = @($mainSource)
    $extraCFlags = @()
    $linkFlags = $windowsLdFlags

    switch ($kind) {
        "generic" { $sources += $runtimeSources }
        "wtf" { $sources += $runtimeSources + $tlsSources + $cryptoSources + @("src/platform/windows/tls.c"); $linkFlags = $windowsTlsLdFlags }
        "image" { $sources += $imageSources + $runtimeSources }
        "bignum" { $extraCFlags += "-Wno-pedantic"; $sources += @("src/shared/bignum.c") + $runtimeSources }
        "hash" { $sources += $hashSources + $runtimeSources }
        "regex" { $sources += $runtimeSources }
        "archive" { $sources += $archiveSources + $runtimeSources }
        "awk" { $sources += $awkSources + $runtimeSources }
        "xml" { $sources += $xmlSources + $runtimeSources }
        "editor" { $sources += $editorSources + $runtimeSources }
        "mail" { $sources += $mailSources + $runtimeSources; $linkFlags = $windowsTlsLdFlags }
        "wget" { $sources += $runtimeSources + $tlsSources + $cryptoSources + @("src/platform/windows/tls.c"); $linkFlags = $windowsTlsLdFlags }
        "ncc" { $extraCFlags += "-Isrc/compiler"; $sources += $nccSources + @("src/platform/windows/core.c") }
        "shell" { $sources += $shellToolSources + @("src/platform/windows/core.c") }
        "make" { $sources += $makeToolSources + @("src/platform/windows/core.c") }
        "httpd" { $sources += $httpdSources + @("src/platform/windows/core.c") }
        "service" { $sources += $serviceSources + @("src/platform/windows/core.c") }
        "ssh" { $sources += $sshSources + @("src/platform/windows/core.c"); $linkFlags = $windowsTlsLdFlags }
        "sshd" { $sources += $sshdSources + @("src/platform/windows/core.c"); $linkFlags = $windowsTlsLdFlags }
        default { throw "Unhandled build kind '$kind' for $tool" }
    }

    Invoke-CompileTool $compilerPath $TargetTriple $tool (Add-Unique $sources) $extraCFlags $linkFlags $output -ShowCommand:$VerboseCommands
}

Write-Output "Built $($selectedTools.Count) Windows freestanding tool(s) in $BuildDir"