param(
    [string]$Compiler = "clang",
    [string]$TargetTriple = "",
    [string]$BuildDir = "",
    [string]$MsysRoot = "C:\msys64",
    [string[]]$Tools = @(),
    [int]$Jobs = 0,
    [int]$LinkJobs = 0,
    [switch]$Clean,
    [switch]$VerboseCommands
)

$ErrorActionPreference = "Stop"

function Find-CommandPath([string]$Name) {
    if ([System.IO.Path]::IsPathRooted($Name) -or $Name.Contains("\") -or $Name.Contains("/")) {
        if (Test-Path -LiteralPath $Name) { return (Resolve-Path -LiteralPath $Name).Path }
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
        if (Test-Path -LiteralPath $candidate) { return $candidate }
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

function Get-WindowsTargetArchitecture([string]$Triple) {
    if ($Triple -match '^(aarch64|arm64)-') { return "aarch64" }
    if ($Triple -match '^(x86_64|amd64)-') { return "x86_64" }
    throw "Unsupported Windows target triple '$Triple'; expected an aarch64/arm64 or x86_64/amd64 target"
}

function New-WindowsImportLibraries(
    [string]$CompilerDirectory,
    [string]$Architecture,
    [string]$OutputDirectory
) {
    $dllTool = Join-Path $CompilerDirectory "llvm-dlltool.exe"
    if (-not (Test-Path -LiteralPath $dllTool)) {
        $dllTool = Find-CommandPath "llvm-dlltool"
    }
    if (-not $dllTool) {
        throw "Could not find llvm-dlltool beside clang or on PATH"
    }

    $machine = if ($Architecture -eq "aarch64") { "arm64" } else { "i386:x86-64" }
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $dllToolFile = Get-Item -LiteralPath $dllTool
    foreach ($library in @("kernel32", "ws2_32", "bcrypt")) {
        $definition = "src/platform/windows/imports/$library.def"
        $archive = Join-Path $OutputDirectory "lib$library.a"
        $signaturePath = "$archive.cmd"
        $signature = "$($dllToolFile.FullName)|$($dllToolFile.Length)|$($dllToolFile.LastWriteTimeUtc.Ticks)|$machine|$definition"
        if ((Test-Path -LiteralPath $archive) -and (Test-Path -LiteralPath $signaturePath) -and
            [IO.File]::ReadAllText([IO.Path]::GetFullPath($signaturePath)) -eq $signature -and
            (Get-Item -LiteralPath $archive).LastWriteTimeUtc -ge (Get-Item -LiteralPath $definition).LastWriteTimeUtc) {
            continue
        }
        & $dllTool -m $machine -d $definition -l $archive
        if ($LASTEXITCODE -ne 0) { throw "llvm-dlltool failed while creating $archive" }
        [IO.File]::WriteAllText([IO.Path]::GetFullPath($signaturePath), $signature)
    }
}

function Assert-SourceFilesExist([string[]]$Sources) {
    foreach ($source in $Sources) {
        if (-not (Test-Path -LiteralPath $source)) { throw "Missing source file: $source" }
    }
}

function Get-TextHash([string]$Text) {
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
        return ([BitConverter]::ToString($sha256.ComputeHash($bytes))).Replace("-", "").ToLowerInvariant()
    } finally {
        $sha256.Dispose()
    }
}

function ConvertTo-ResponseArgument([string]$Argument) {
    $Argument = $Argument.Replace('\', '/')
    if ($Argument -notmatch '[\s"]') { return $Argument }
    return '"' + $Argument.Replace('"', '\"') + '"'
}

function Write-ResponseFile([string]$Path, [string[]]$Arguments) {
    $directory = Split-Path -Parent $Path
    if ($directory) { New-Item -ItemType Directory -Force $directory | Out-Null }
    $lines = @($Arguments | ForEach-Object { ConvertTo-ResponseArgument $_ })
    [IO.File]::WriteAllLines([IO.Path]::GetFullPath($Path), $lines, [Text.UTF8Encoding]::new($false))
}

function Test-ObjectCache(
    [string]$ObjectPath,
    [string]$DependencyPath,
    [string]$SignaturePath,
    [string]$Signature
) {
    if (-not (Test-Path -LiteralPath $ObjectPath) -or -not (Test-Path -LiteralPath $DependencyPath) -or -not (Test-Path -LiteralPath $SignaturePath)) { return $false }
    if ([IO.File]::ReadAllText([IO.Path]::GetFullPath($SignaturePath)) -ne $Signature) { return $false }

    $objectTime = (Get-Item -LiteralPath $ObjectPath).LastWriteTimeUtc
    $dependencyText = [IO.File]::ReadAllText([IO.Path]::GetFullPath($DependencyPath))
    $dependencyText = [regex]::Replace($dependencyText, "\\\r?\n", " ")
    $separator = $dependencyText.IndexOf(':')
    if ($separator -lt 0) { return $false }
    $dependencies = @($dependencyText.Substring($separator + 1) -split '\s+' | Where-Object { $_ -ne "" })
    if ($dependencies.Count -eq 0) { return $false }
    foreach ($dependency in $dependencies) {
        $dependency = $dependency.Replace('\ ', ' ')
        if (-not (Test-Path -LiteralPath $dependency) -or (Get-Item -LiteralPath $dependency).LastWriteTimeUtc -gt $objectTime) {
            return $false
        }
    }
    return $true
}

function Test-LinkCache(
    [string]$OutputPath,
    [string[]]$Inputs,
    [string]$SignaturePath,
    [string]$Signature
) {
    if (-not (Test-Path -LiteralPath $OutputPath) -or -not (Test-Path -LiteralPath $SignaturePath)) { return $false }
    if ([IO.File]::ReadAllText([IO.Path]::GetFullPath($SignaturePath)) -ne $Signature) { return $false }
    $outputTime = (Get-Item -LiteralPath $OutputPath).LastWriteTimeUtc
    foreach ($input in $Inputs) {
        if (-not (Test-Path -LiteralPath $input) -or (Get-Item -LiteralPath $input).LastWriteTimeUtc -gt $outputTime) { return $false }
    }
    return $true
}

function Invoke-ParallelCompilerCommands(
    [object[]]$Commands,
    [int]$MaximumJobs,
    [string]$CompilerPath,
    [string]$WorkingDirectory,
    [string]$Phase,
    [switch]$ShowCommands
) {
    if ($Commands.Count -eq 0) { return }
    $ordered = @($Commands | Sort-Object -Property @{ Expression = { $_.Cost }; Descending = $true }, Label)
    $completed = 0
    $nextIndex = 0
    $running = New-Object System.Collections.Generic.List[object]
    $failures = New-Object System.Collections.Generic.List[string]
    while ($running.Count -gt 0 -or ($failures.Count -eq 0 -and $nextIndex -lt $ordered.Count)) {
        while ($failures.Count -eq 0 -and $running.Count -lt $MaximumJobs -and $nextIndex -lt $ordered.Count) {
            $command = $ordered[$nextIndex]
            $nextIndex += 1
            Write-ResponseFile $command.ResponsePath $command.Arguments
            if ($ShowCommands) {
                Write-Output ("{0} {1}" -f $CompilerPath, ($command.Arguments -join " "))
            }
            $startInfo = [Diagnostics.ProcessStartInfo]::new()
            $startInfo.FileName = $CompilerPath
            $startInfo.Arguments = '@"' + [IO.Path]::GetFullPath($command.ResponsePath) + '"'
            $startInfo.WorkingDirectory = $WorkingDirectory
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true
            $process = [Diagnostics.Process]::new()
            $process.StartInfo = $startInfo
            if (-not $process.Start()) { throw "failed to start clang for $($command.Label)" }
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            $running.Add([pscustomobject]@{ Command = $command; Process = $process; Stdout = $stdout; Stderr = $stderr })
        }

        if ($running.Count -eq 0) { continue }
        $outputTasks = [Threading.Tasks.Task[]]@($running | ForEach-Object { $_.Stdout })
        $finishedOutput = [Threading.Tasks.Task]::WhenAny($outputTasks).GetAwaiter().GetResult()
        for ($index = $running.Count - 1; $index -ge 0; --$index) {
            $item = $running[$index]
            if ($item.Stdout -ne $finishedOutput -and -not $item.Process.HasExited) { continue }
            $item.Process.WaitForExit()
            $standardOutput = $item.Stdout.Result
            $standardError = $item.Stderr.Result
            if ($standardOutput) { Write-Output $standardOutput.TrimEnd() }
            if ($standardError) { [Console]::Error.WriteLine($standardError.TrimEnd()) }
            if ($item.Process.ExitCode -ne 0) {
                $failures.Add($item.Command.Label)
            } else {
                [IO.File]::WriteAllText([IO.Path]::GetFullPath($item.Command.SignaturePath), $item.Command.Signature)
            }
            $item.Process.Dispose()
            $running.RemoveAt($index)
            $completed += 1
            if (-not $ShowCommands) { Write-Output ("[{0} {1}/{2}] {3}" -f $Phase, $completed, $ordered.Count, $item.Command.Label) }
        }
    }
    if ($failures.Count -ne 0) { throw "clang failed during $Phase for: $($failures -join ', ')" }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Push-Location $repoRoot
try {

$compilerPath = Find-Clang $Compiler $MsysRoot
if (-not $compilerPath) {
    throw "Could not find '$Compiler'. Install LLVM/Clang for Windows or pass -Compiler <path-to-clang.exe>."
}

if ([string]::IsNullOrWhiteSpace($TargetTriple)) {
    $TargetTriple = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "aarch64-w64-windows-gnu" } else { "x86_64-w64-windows-gnu" }
}
$targetArchitecture = Get-WindowsTargetArchitecture $TargetTriple
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = "build/freestanding-windows-$targetArchitecture"
}
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }
if ($LinkJobs -le 0) { $LinkJobs = [Math]::Min($Jobs, 4) }
if ($Jobs -lt 1 -or $LinkJobs -lt 1) { throw "Jobs and LinkJobs must be positive" }

$compilerDir = Split-Path -Parent $compilerPath
if ($env:PATH -notlike "*$compilerDir*") {
    $env:PATH = $compilerDir + ";" + $env:PATH
}

$makefileText = Get-Content "Makefile" -Raw
$manifestText = Get-Content "src/compiler/source_manifest.h" -Raw

$allTools = Read-MakeVariable $makefileText "TOOLS"
$selectedTools = if ($Tools.Count -gt 0) { $Tools } else { Remove-Tools $allTools @("ncc") }
$unknownTools = @($selectedTools | Where-Object { $allTools -notcontains $_ })
if ($unknownTools.Count -gt 0) { throw "Unknown tool(s): $($unknownTools -join ', ')" }

$compilerSources = @(Read-ManifestSources $manifestText "FOREACH_COMPILER_SOURCE")
$sharedSources = @(Read-ManifestSources $manifestText "FOREACH_SHARED_SOURCE")
$imageManifestSources = @(Read-ManifestSources $manifestText "FOREACH_IMAGE_SOURCE")
$pgpManifestSources = @(Read-ManifestSources $manifestText "FOREACH_PGP_SOURCE")
$pdfManifestSources = @(Read-ManifestSources $manifestText "FOREACH_PDF_SOURCE")
$cryptoSources = @(Read-ManifestSources $manifestText "FOREACH_CRYPTO_SOURCE")
$tlsSources = @(Read-ManifestSources $manifestText "FOREACH_TLS_SOURCE")
$tuiSources = @(Read-ManifestSources $manifestText "FOREACH_TUI_SOURCE")
$usbSources = @(Read-ManifestSources $manifestText "FOREACH_USB_SOURCE")
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

$runtimeSources = Add-Unique (Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_RUNTIME_SOURCES")
$stackProbeSource = "src/arch/$targetArchitecture/windows/chkstk.S"
$windowsRandomSource = "src/platform/windows/random.c"
$windowsTlsSources = @("src/platform/windows/tls.c", $windowsRandomSource)
$imageSources = Add-Unique (@($imageManifestSources) + @("src/shared/compression/crc32.c", "src/shared/compression/zlib.c"))
$pgpSources = Add-Unique (@($pgpManifestSources) + @("src/shared/crypto/rsa.c", $windowsRandomSource))
$pgpquerySources = Add-Unique (@($pgpManifestSources) + @($tlsSources) + @($cryptoSources) + @($windowsTlsSources))
$pdfSources = Add-Unique (@($pdfManifestSources) + @("src/shared/compression/zlib.c"))
$hashSources = @("src/shared/hash_util.c", "src/shared/crypto/md5.c", "src/shared/crypto/sha1.c", "src/shared/crypto/sha256.c", "src/shared/crypto/sha512.c")
$archiveSources = @(
    "src/shared/archive_util.c", "src/shared/object_util.c", "src/shared/archive_zip.c",
    "src/shared/compression/bzip2.c", "src/shared/compression/crc32.c",
    "src/shared/compression/lzss.c", "src/shared/compression/zlib.c",
    "src/shared/crypto/sha256.c"
)
$awkSources = @("src/tools/awk/awk_parse.c", "src/tools/awk/awk_exec.c")
$xmlSources = @("src/shared/xml.c", "src/shared/xml_stream.c", "src/shared/xml_dtd.c", "src/shared/tool_xml.c")
$editorSources = Add-Unique (@($variables["EDITOR_TOOL_SOURCES"]) + @($tuiSources))
$mailSources = Add-Unique (@($variables["MAIL_TOOL_SOURCES"]) + @($tuiSources) + @($tlsSources) + @($cryptoSources) + @($windowsTlsSources))
$nccSources = Add-Unique (@($compilerSources) + @($sharedSources) + @("src/shared/crypto/sha256.c"))
$linkerSources = Add-Unique (@($compilerSources | Where-Object { $_ -match 'src/compiler/linker[^/]*\.c$' }) + @(
    "src/shared/compression/lzss.c", "src/shared/crypto/sha256.c"
))
$shellToolSources = Add-Unique (@($shellSources) + @($sharedSources))
$makeToolSources = Add-Unique (@($variables["MAKE_TOOL_SOURCES"]) + @($sharedSources))
$httpdSources = Add-Unique (@($variables["HTTPD_TOOL_SOURCES"]) + @($sharedSources))
$serviceSources = Add-Unique (@($variables["SERVICE_TOOL_SOURCES"]) + @($sharedSources))
$sshTransportSources = @($sshClientSources | Where-Object { $_ -match 'src/shared/ssh/ssh_(core|client_io)\.c$' })
$sshCryptoSources = Add-Unique (@($cryptoSources) + @("src/shared/crypto/curve25519.c", "src/shared/crypto/ed25519.c", "src/shared/crypto/chacha20_poly1305.c", "src/shared/crypto/ssh_kdf.c"))
$sshSources = Add-Unique (@($sshClientSources) + @($sshCryptoSources) + @($tlsSources) + @($windowsTlsSources) + @($sharedSources))
$sshdSources = Add-Unique (@($sshdToolSources) + @($sshTransportSources) + @($sshCryptoSources) + @($tlsSources) + @($windowsTlsSources) + @($sharedSources))
$gitSources = Add-Unique (@($sshClientSources) + @($sshCryptoSources) + @($tlsSources) + @("src/shared/compression/crc32.c", "src/shared/compression/zlib.c") + @($windowsTlsSources))
$windowsUsbSources = Add-Unique (@($usbSources) + @("src/platform/windows/usb.c"))

$bignumTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_BIGNUM_TOOLS"
$hashTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_HASH_TOOLS"
$imageTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_IMAGE_TOOLS"
$pgpTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_PGP_TOOLS"
$pgpqueryTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_PGPQUERY_TOOLS"
$pdfTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_PDF_TOOLS"
$regexTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_REGEX_TOOLS"
$archiveTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_ARCHIVE_TOOLS"
$awkTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_AWK_TOOLS"
$xmlTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_XML_TOOLS"
$tuiTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_TUI_TOOLS"
$mailTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_MAIL_TOOLS"
$wgetTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_WGET_TOOLS"
$windowsTlsTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_TLS_TOOLS"
$minimalTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_MINIMAL_TOOLS"
$nccTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_NCC_TOOLS"
$linkerTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_LINKER_TOOLS"
$shellTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_SHELL_TOOLS"
$makeTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_MAKE_TOOLS"
$httpdTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_HTTPD_TOOLS"
$serviceTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_SERVICE_TOOLS"
$sshTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_SSH_TOOLS"
$sshdTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_SSHD_TOOLS"
$gitTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_GIT_TOOLS"
$usbTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_USB_TOOLS"
$aliasTools = Read-MakeVariable $makefileText "WINDOWS_FREESTANDING_ALIAS_TOOLS"

$specialTools = Add-Unique ($imageTools + $pgpTools + $pgpqueryTools + $pdfTools + $bignumTools + $hashTools + $regexTools + $archiveTools + $awkTools + $xmlTools + $tuiTools + $mailTools + $wgetTools + $windowsTlsTools + $minimalTools + $nccTools + $linkerTools + $shellTools + $makeTools + $httpdTools + $serviceTools + $sshTools + $sshdTools + $gitTools + $usbTools + $aliasTools)
$genericTools = Remove-Tools $allTools $specialTools

$toolKinds = @{}
foreach ($tool in $genericTools) { $toolKinds[$tool] = "generic" }
foreach ($tool in $imageTools) { $toolKinds[$tool] = "image" }
foreach ($tool in $pgpTools) { $toolKinds[$tool] = "pgp" }
foreach ($tool in $pgpqueryTools) { $toolKinds[$tool] = "pgpquery" }
foreach ($tool in $pdfTools) { $toolKinds[$tool] = "pdf" }
foreach ($tool in $bignumTools) { $toolKinds[$tool] = "bignum" }
foreach ($tool in $hashTools) { $toolKinds[$tool] = "hash" }
foreach ($tool in $regexTools) { $toolKinds[$tool] = "regex" }
foreach ($tool in $archiveTools) { $toolKinds[$tool] = "archive" }
foreach ($tool in $awkTools) { $toolKinds[$tool] = "awk" }
foreach ($tool in $xmlTools) { $toolKinds[$tool] = "xml" }
foreach ($tool in $tuiTools) { $toolKinds[$tool] = "editor" }
foreach ($tool in $mailTools) { $toolKinds[$tool] = "mail" }
foreach ($tool in $wgetTools) { $toolKinds[$tool] = "wget" }
foreach ($tool in $windowsTlsTools) { $toolKinds[$tool] = "tls" }
foreach ($tool in $minimalTools) { $toolKinds[$tool] = "minimal" }
foreach ($tool in $nccTools) { $toolKinds[$tool] = "ncc" }
foreach ($tool in $linkerTools) { $toolKinds[$tool] = "linker" }
foreach ($tool in $shellTools) { $toolKinds[$tool] = "shell" }
foreach ($tool in $makeTools) { $toolKinds[$tool] = "make" }
foreach ($tool in $httpdTools) { $toolKinds[$tool] = "httpd" }
foreach ($tool in $serviceTools) { $toolKinds[$tool] = "service" }
foreach ($tool in $sshTools) { $toolKinds[$tool] = "ssh" }
foreach ($tool in $sshdTools) { $toolKinds[$tool] = "sshd" }
foreach ($tool in $gitTools) { $toolKinds[$tool] = "git" }
foreach ($tool in $usbTools) { $toolKinds[$tool] = "usb" }
foreach ($tool in $aliasTools) { $toolKinds[$tool] = "alias" }

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$importLibraryDir = Join-Path $BuildDir ".imports"
New-WindowsImportLibraries $compilerDir $targetArchitecture $importLibraryDir

$script:WindowsCFlags = @(
    "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Oz",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-ffunction-sections", "-fdata-sections", "-flto",
    "-Isrc/shared", "-Isrc/platform/windows"
)
$windowsLdFlags = @("-nostdlib", "-fuse-ld=lld", "-Wl,-e,mainCRTStartup", "-Wl,-s", "-Wl,--gc-sections", "-Wl,--icf=safe", "-Wl,--no-insert-timestamp", "-Wl,/merge:.rdata=.text", "-Wl,--stack,8388608", "-L$importLibraryDir", "-lkernel32", "-lws2_32")
$windowsTlsLdFlags = $windowsLdFlags + @("-lbcrypt")

$objectRoot = Join-Path $BuildDir ".objects"
$commandRoot = Join-Path $BuildDir ".commands"
New-Item -ItemType Directory -Force $objectRoot, $commandRoot | Out-Null
$compilerFile = Get-Item -LiteralPath $compilerPath
$compilerIdentity = "$($compilerFile.FullName)|$($compilerFile.Length)|$($compilerFile.LastWriteTimeUtc.Ticks)"
$compileCommands = New-Object System.Collections.Generic.List[object]
$toolPlans = New-Object System.Collections.Generic.List[object]
$aliasPlans = New-Object System.Collections.Generic.List[object]
$objectRecords = @{}
$reusedObjectCount = 0
Write-Output "Compiler: $compilerPath"
Write-Output "Target:   $TargetTriple"
Write-Output "Output:   $BuildDir"
Write-Output "Tools:    $($selectedTools.Count)"
Write-Output "Jobs:     $Jobs compile, $LinkJobs link"

foreach ($tool in $selectedTools) {
    if (-not $toolKinds.ContainsKey($tool)) { throw "No Windows freestanding build rule for $tool" }
    $output = Join-Path $BuildDir "$tool.exe"
    $kind = $toolKinds[$tool]

    if ($kind -eq "alias") {
        if ($tool -eq "ping6") {
            $aliasPlans.Add([pscustomobject]@{ Tool = $tool; Source = Join-Path $BuildDir "ping.exe"; Output = $output })
            continue
        }
        if ($tool -eq "rg") {
            $aliasPlans.Add([pscustomobject]@{ Tool = $tool; Source = Join-Path $BuildDir "ripgrep.exe"; Output = $output })
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
        "tls" { $sources += $runtimeSources + $tlsSources + $cryptoSources + @($windowsTlsSources); $linkFlags = $windowsTlsLdFlags }
        "image" { $sources += $imageSources + $runtimeSources }
        "pgp" { $sources += $pgpSources + $runtimeSources; $linkFlags = $windowsTlsLdFlags }
        "pgpquery" { $sources += $pgpquerySources + $runtimeSources; $linkFlags = $windowsTlsLdFlags }
        "pdf" { $sources += $pdfSources + $runtimeSources }
        "bignum" { $extraCFlags += "-Wno-pedantic"; $sources += @("src/shared/bignum.c") + $runtimeSources }
        "hash" { $sources += $hashSources + $runtimeSources }
        "regex" { $sources += $runtimeSources }
        "archive" { $sources += $archiveSources + $runtimeSources }
        "awk" { $sources += $awkSources + $runtimeSources }
        "xml" { $sources += $xmlSources + $runtimeSources }
        "editor" { $sources += $editorSources + $runtimeSources }
        "mail" { $sources += $mailSources + $runtimeSources; $linkFlags = $windowsTlsLdFlags }
        "wget" { $sources += $runtimeSources + $tlsSources + $cryptoSources + @($windowsTlsSources); $linkFlags = $windowsTlsLdFlags }
        "minimal" { $sources += "src/platform/windows/minimal_start.c" }
        "ncc" { $extraCFlags += "-Isrc/compiler"; $sources += $nccSources + @("src/platform/windows/core.c") }
        "linker" { $extraCFlags += "-Isrc/compiler"; $sources += $linkerSources + $runtimeSources }
        "shell" { $sources += $shellToolSources + @("src/platform/windows/core.c") }
        "make" { $sources += $makeToolSources + @("src/platform/windows/core.c") }
        "httpd" { $sources += $httpdSources + @("src/platform/windows/core.c") }
        "service" { $sources += $serviceSources + @("src/platform/windows/core.c") }
        "ssh" { $sources += $sshSources + @("src/platform/windows/core.c"); $linkFlags = $windowsTlsLdFlags }
        "sshd" { $sources += $sshdSources + @("src/platform/windows/core.c"); $linkFlags = $windowsTlsLdFlags }
        "git" { $sources += $gitSources + $runtimeSources; $linkFlags = $windowsTlsLdFlags }
        "usb" { $sources += $windowsUsbSources + $runtimeSources }
        default { throw "Unhandled build kind '$kind' for $tool" }
    }

    if ($kind -ne "minimal") {
        $sources += $stackProbeSource
    }
    $sources = Add-Unique $sources
    Assert-SourceFilesExist $sources
    $compileFlags = @("--target=$TargetTriple") + $script:WindowsCFlags + $extraCFlags
    $profileHash = (Get-TextHash ($compilerIdentity + "`n" + ($compileFlags -join "`n"))).Substring(0, 16)
    $profileDirectory = Join-Path $objectRoot $profileHash
    New-Item -ItemType Directory -Force $profileDirectory | Out-Null
    $objects = New-Object System.Collections.Generic.List[string]
    foreach ($source in $sources) {
        $sourceKey = "$profileHash|$($source.ToLowerInvariant())"
        if (-not $objectRecords.ContainsKey($sourceKey)) {
            $sourceHash = (Get-TextHash $source).Substring(0, 12)
            $baseName = [IO.Path]::GetFileNameWithoutExtension($source) -replace '[^A-Za-z0-9_.-]', '_'
            $objectPath = Join-Path $profileDirectory "$baseName-$sourceHash.obj"
            $dependencyPath = "$objectPath.d"
            $signaturePath = "$objectPath.cmd"
            $arguments = $compileFlags + @("-c", $source, "-MMD", "-MF", $dependencyPath, "-MT", "__object__", "-o", $objectPath)
            $signature = $compilerIdentity + "`n" + ($arguments -join "`n")
            $objectRecord = [pscustomobject]@{ Path = $objectPath; DependencyPath = $dependencyPath; SignaturePath = $signaturePath }
            $objectRecords[$sourceKey] = $objectRecord
            if (Test-ObjectCache $objectPath $dependencyPath $signaturePath $signature) {
                $reusedObjectCount += 1
            } else {
                $compileCommands.Add([pscustomobject]@{
                    Label = $source
                    Arguments = $arguments
                    ResponsePath = Join-Path $commandRoot "compile-$profileHash-$sourceHash.rsp"
                    SignaturePath = $signaturePath
                    Signature = $signature
                    Cost = (Get-Item -LiteralPath $source).Length
                })
            }
        }
        $objects.Add($objectRecords[$sourceKey].Path)
    }
    $toolPlans.Add([pscustomobject]@{
        Tool = $tool
        Output = $output
        Objects = @($objects | ForEach-Object { $_ })
        LinkFlags = @($linkFlags)
    })
}

$compileCommandArray = @($compileCommands | ForEach-Object { $_ })
Invoke-ParallelCompilerCommands $compileCommandArray $Jobs $compilerPath $repoRoot "compile" -ShowCommands:$VerboseCommands

$importInputs = @(Get-ChildItem $importLibraryDir -Filter "*.a" | ForEach-Object { $_.FullName })
$linkCommands = New-Object System.Collections.Generic.List[object]
$reusedLinkCount = 0
foreach ($plan in $toolPlans) {
    $arguments = @("--target=$TargetTriple") + @($plan.Objects) + @($plan.LinkFlags) + @("-o", $plan.Output)
    $signaturePath = "$($plan.Output).cmd"
    $signature = $compilerIdentity + "`n" + ($arguments -join "`n")
    if (Test-LinkCache $plan.Output (@($plan.Objects) + $importInputs) $signaturePath $signature) {
        $reusedLinkCount += 1
    } else {
        $linkCommands.Add([pscustomobject]@{
            Label = $plan.Tool
            Arguments = $arguments
            ResponsePath = Join-Path $commandRoot "link-$($plan.Tool -replace '[^A-Za-z0-9_.-]', '_').rsp"
            SignaturePath = $signaturePath
            Signature = $signature
            Cost = $plan.Objects.Count
        })
    }
}
$linkCommandArray = @($linkCommands | ForEach-Object { $_ })
Invoke-ParallelCompilerCommands $linkCommandArray $LinkJobs $compilerPath $repoRoot "link" -ShowCommands:$VerboseCommands

foreach ($alias in $aliasPlans) {
    if (-not (Test-Path -LiteralPath $alias.Source)) { throw "Cannot create $($alias.Tool) before $($alias.Source) exists" }
    if (-not (Test-Path -LiteralPath $alias.Output) -or (Get-Item -LiteralPath $alias.Source).LastWriteTimeUtc -gt (Get-Item -LiteralPath $alias.Output).LastWriteTimeUtc) {
        Copy-Item -LiteralPath $alias.Source -Destination $alias.Output -Force
    }
}

Write-Output "Built $($selectedTools.Count) Windows freestanding tool(s) in $BuildDir"
Write-Output "Objects: $($compileCommands.Count) compiled, $reusedObjectCount reused; links: $($linkCommands.Count) linked, $reusedLinkCount reused"
} finally {
    Pop-Location
}
