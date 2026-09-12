param(
    [string]$BuildDir = "experimental/snapdragon/build",
    [string]$Version = "1.24.4"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$packageName = "microsoft.ml.onnxruntime.qnn.$Version.nupkg"
$packagePath = Join-Path $env:TEMP $packageName
$packageUrl = "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.qnn/$Version/$packageName"
$testedVersion = "1.24.4"
$testedSha256 = "E4D6EABB9E503D4F3C78494FC9400F02509B2EE315D9F707644A174ECE8DA17F"

Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

    if (-not (Test-Path -LiteralPath $packagePath)) {
        Write-Output "Downloading Microsoft.ML.OnnxRuntime.QNN $Version"
        Invoke-WebRequest -Uri $packageUrl -OutFile $packagePath
    }

    if ($Version -eq $testedVersion -and $testedSha256) {
        $actualSha256 = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
        if ($actualSha256 -ne $testedSha256) {
            throw "QNN package SHA-256 mismatch: expected $testedSha256, got $actualSha256"
        }
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($packagePath)
    try {
        $runtimePrefix = "runtimes/win-arm64/native/"
        $runtimeNames = @(
            "QnnHtp.dll",
            "QnnHtpPrepare.dll",
            "QnnHtpV73Stub.dll",
            "QnnHtpV81Stub.dll",
            "QnnSystem.dll",
            "libQnnHtpV73Skel.so",
            "libQnnHtpV81Skel.so",
            "libqnnhtpv73.cat",
            "libqnnhtpv81.cat"
        )

        foreach ($name in $runtimeNames) {
            $entry = $archive.GetEntry($runtimePrefix + $name)
            if (-not $entry) { throw "Package is missing $name" }
            $target = Join-Path $BuildDir $name
            $inputStream = $entry.Open()
            $outputStream = [IO.File]::Create($target)
            try {
                $inputStream.CopyTo($outputStream)
            } finally {
                $outputStream.Dispose()
                $inputStream.Dispose()
            }
            Write-Output "Staged $target"
        }

        $licenseDir = Join-Path $BuildDir "qnn-licenses"
        New-Item -ItemType Directory -Force -Path $licenseDir | Out-Null
        foreach ($name in @("LICENSE", "Qualcomm_LICENSE.pdf", "ThirdPartyNotices.txt")) {
            $entry = $archive.GetEntry($name)
            if (-not $entry) { throw "Package is missing $name" }
            $target = Join-Path $licenseDir $name
            $inputStream = $entry.Open()
            $outputStream = [IO.File]::Create($target)
            try {
                $inputStream.CopyTo($outputStream)
            } finally {
                $outputStream.Dispose()
                $inputStream.Dispose()
            }
        }
    } finally {
        $archive.Dispose()
    }

    Write-Output "Staged the direct QNN HTP runtime without ONNX Runtime."
} finally {
    Pop-Location
}