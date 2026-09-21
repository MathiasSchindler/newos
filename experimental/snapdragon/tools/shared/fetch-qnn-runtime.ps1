param(
    [string]$BuildDir = "experimental/snapdragon/build",
    [string]$SdkArchive = "experimental/snapdragon/data/v2.50.0.260828.zip"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path
$sdkVersion = "2.50.0.260828"
$sdkRoot = "qairt/$sdkVersion"
$expectedSha256 = "A346EA0E2C8631B46D57261A4969994CD9CC34124A8355BBC7B08B2C8BD859A5"

function Copy-ArchiveEntry {
    param(
        [IO.Compression.ZipArchive]$Archive,
        [string]$Source,
        [string]$Target
    )

    $entry = $Archive.GetEntry($Source)
    if (-not $entry) { throw "QAIRT SDK is missing $Source" }
    $inputStream = $entry.Open()
    $outputStream = [IO.File]::Create($Target)
    try {
        $inputStream.CopyTo($outputStream)
    } finally {
        $outputStream.Dispose()
        $inputStream.Dispose()
    }
}

function Read-ArchiveEntry {
    param(
        [IO.Compression.ZipArchive]$Archive,
        [string]$Source
    )

    $entry = $Archive.GetEntry($Source)
    if (-not $entry) { throw "QAIRT SDK is missing $Source" }
    $stream = $entry.Open()
    $reader = [IO.StreamReader]::new($stream)
    try {
        return $reader.ReadToEnd()
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

Push-Location $repoRoot
try {
    if (-not [IO.Path]::IsPathRooted($SdkArchive)) {
        $SdkArchive = Join-Path $repoRoot $SdkArchive
    }
    if (-not (Test-Path -LiteralPath $SdkArchive -PathType Leaf)) {
        throw "QAIRT SDK archive not found: $SdkArchive"
    }

    $actualSha256 = (Get-FileHash -LiteralPath $SdkArchive -Algorithm SHA256).Hash
    if ($actualSha256 -ne $expectedSha256) {
        throw "QAIRT SDK SHA-256 mismatch: expected $expectedSha256, got $actualSha256"
    }

    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    $stageDir = Join-Path $BuildDir ".qnn-runtime-stage"
    if (Test-Path -LiteralPath $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($SdkArchive)
    try {
        $sdkMetadata = Read-ArchiveEntry $archive "$sdkRoot/sdk.yaml"
        $commonHeader = Read-ArchiveEntry $archive "$sdkRoot/include/QNN/QnnCommon.h"
        if ($sdkMetadata -notmatch '(?m)^version:\s+2\.50\.0\s*$' -or
            $sdkMetadata -notmatch '(?m)^build_id:\s+260828221209\s*$' -or
            $sdkMetadata -notmatch '(?m)^qnn_backend_api_version:\s+2\.18\.0\s*$') {
            throw "QAIRT SDK metadata does not match the pinned 2.50 release"
        }
        if ($commonHeader -notmatch '(?m)^#define\s+QNN_API_VERSION_MAJOR\s+2\s*$' -or
            $commonHeader -notmatch '(?m)^#define\s+QNN_API_VERSION_MINOR\s+39\s*$' -or
            $commonHeader -notmatch '(?m)^#define\s+QNN_API_VERSION_PATCH\s+0\s*$') {
            throw "QAIRT SDK does not expose the expected QNN core API 2.39.0"
        }

        $runtimeEntries = @(
            @("$sdkRoot/lib/aarch64-windows-msvc/QnnHtp.dll", "QnnHtp.dll"),
            @("$sdkRoot/lib/aarch64-windows-msvc/QnnHtpPrepare.dll", "QnnHtpPrepare.dll"),
            @("$sdkRoot/lib/aarch64-windows-msvc/QnnHtpV73Stub.dll", "QnnHtpV73Stub.dll"),
            @("$sdkRoot/lib/hexagon-v73/unsigned/libQnnHtpV73Skel.so", "libQnnHtpV73Skel.so"),
            @("$sdkRoot/lib/hexagon-v73/unsigned/libqnnhtpv73.cat", "libqnnhtpv73.cat")
        )
        foreach ($runtimeEntry in $runtimeEntries) {
            Copy-ArchiveEntry $archive $runtimeEntry[0] (Join-Path $stageDir $runtimeEntry[1])
        }

        $licenseStageDir = Join-Path $stageDir "qnn-licenses"
        New-Item -ItemType Directory -Force -Path $licenseStageDir | Out-Null
        foreach ($name in @("LICENSE.pdf", "NOTICE.txt", "NOTICE_WINDOWS.txt", "sdk.yaml")) {
            Copy-ArchiveEntry $archive "$sdkRoot/$name" (Join-Path $licenseStageDir $name)
        }
    } finally {
        $archive.Dispose()
    }

    $manifest = @(
        "QAIRT SDK: $sdkVersion",
        "QAIRT build: 260828221209",
        "QNN core API: 2.39.0",
        "sdk.yaml qnn_backend_api_version: 2.18.0",
        "SDK archive SHA-256: $actualSha256"
    )
    foreach ($file in Get-ChildItem -LiteralPath $stageDir -File) {
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        $manifest += "$($file.Name) SHA-256: $hash"
    }
    [IO.File]::WriteAllLines((Join-Path $stageDir "qnn-runtime-version.txt"), $manifest)

    $licenseDir = Join-Path $BuildDir "qnn-licenses"
    if (Test-Path -LiteralPath $licenseDir) {
        Remove-Item -LiteralPath $licenseDir -Recurse -Force
    }
    Move-Item -LiteralPath (Join-Path $stageDir "qnn-licenses") -Destination $licenseDir
    foreach ($runtimeEntry in $runtimeEntries) {
        $target = Join-Path $BuildDir $runtimeEntry[1]
        Move-Item -LiteralPath (Join-Path $stageDir $runtimeEntry[1]) -Destination $target -Force
        Write-Output "Staged $target"
    }
    foreach ($obsolete in @('QnnHtpV81Stub.dll','QnnSystem.dll','libQnnHtpV81Skel.so','libqnnhtpv81.cat')) {
        $target = Join-Path $BuildDir $obsolete
        if (Test-Path -LiteralPath $target) {
            Remove-Item -LiteralPath $target -Force
            Write-Output "Removed obsolete runtime $target"
        }
    }
    Move-Item -LiteralPath (Join-Path $stageDir "qnn-runtime-version.txt") `
        -Destination (Join-Path $BuildDir "qnn-runtime-version.txt") -Force
    Remove-Item -LiteralPath $stageDir -Force

    Write-Output "Staged QAIRT $sdkVersion / QNN core 2.39.0 from the official SDK archive."
} finally {
    Pop-Location
}