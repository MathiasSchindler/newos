param(
    [string]$StageDirectory = 'D:\newos-data\nemotron-asr\m1\fp32-package'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$build = Join-Path $repo 'experimental\snapdragon\build\nemotron-fp32'
$public = Join-Path $repo 'experimental\snapdragon\data\fastrpc-signing\development.cer'
$expected = '2953364187cf4d6758961596aca6441c89c9aae4f30a2eb6bf11b3c7d36689fe'
if ((Get-FileHash -LiteralPath $public -Algorithm SHA256).Hash -ine $expected) {
    throw 'Existing approved development certificate fingerprint differs'
}
Add-Type -AssemblyName System.Security
$identity = New-Object Security.Cryptography.X509Certificates.X509Certificate2($public)
$certificate = Get-Item ('Cert:\CurrentUser\My\' + $identity.Thumbprint)
if (-not $certificate.HasPrivateKey -or $certificate.NotAfter -le (Get-Date)) {
    throw 'Existing development signing identity is not usable'
}
if (-not [IO.Path]::IsPathRooted($StageDirectory)) { throw 'StageDirectory must be absolute' }
if ([IO.Path]::GetFullPath($StageDirectory).StartsWith($build, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Signed artifacts must not be staged inside the disposable build directory'
}
$payload = Join-Path $StageDirectory 'payload'
New-Item -ItemType Directory -Force -Path $payload | Out-Null
if (@(Get-ChildItem -LiteralPath $payload -Force | Where-Object { $_.Name -ne 'libNmFp32V73.so' }).Count) {
    throw 'DSP signing payload contains unexpected files'
}
$dsp = Join-Path $payload 'libNmFp32V73.so'
$runtimeDsp = Join-Path $StageDirectory 'libNmFp32V73.so'
$hostPackage = Join-Path $StageDirectory 'NmFp32Host.dll'
$catalog = Join-Path $StageDirectory 'libNmFp32V73.cat'
Copy-Item -LiteralPath "$build\libNmFp32V73.so" -Destination $dsp -Force
Copy-Item -LiteralPath "$build\NmFp32Host.dll" -Destination $hostPackage -Force
New-FileCatalog -Path $payload -CatalogFilePath $catalog -CatalogVersion 2 | Out-Null
$signature = Set-AuthenticodeSignature -LiteralPath $catalog -Certificate $certificate -HashAlgorithm SHA256 -IncludeChain All
if (-not $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $identity.Thumbprint) {
    throw 'DSP catalog was not signed by the existing development identity'
}
$cms = New-Object Security.Cryptography.Pkcs.SignedCms
$cms.Decode([IO.File]::ReadAllBytes($catalog))
$cms.CheckSignature($true)
if ($cms.SignerInfos.Count -ne 1 -or $cms.SignerInfos[0].Certificate.Thumbprint -ne $identity.Thumbprint -or
    (Test-FileCatalog -Path $payload -CatalogFilePath $catalog) -ne 'Valid') {
    throw 'DSP catalog signature or membership verification failed'
}
Copy-Item -LiteralPath $dsp -Destination $runtimeDsp -Force
$manifest = [ordered]@{
    schema = 1
    package = 'newos.nemotron.fp32'
    interfaceProvider = 'NmFp32InterfaceProvider'
    sdk = '2.50.0.260828'
    architecture = 'v73'
    host = [ordered]@{ path = $hostPackage; sha256 = (Get-FileHash -LiteralPath $hostPackage -Algorithm SHA256).Hash.ToLowerInvariant() }
    dsp = [ordered]@{ path = $runtimeDsp; sha256 = (Get-FileHash -LiteralPath $runtimeDsp -Algorithm SHA256).Hash.ToLowerInvariant() }
    catalog = [ordered]@{ path = $catalog; sha256 = (Get-FileHash -LiteralPath $catalog -Algorithm SHA256).Hash.ToLowerInvariant() }
    certificateThumbprint = $identity.Thumbprint
    certificateSha256 = $expected
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -Encoding ASCII -LiteralPath (Join-Path $StageDirectory 'manifest.json')
Write-Output ($manifest | ConvertTo-Json -Depth 4)
Write-Output 'CATALOG_SIGNATURE_AND_MEMBERSHIP_PASS; no certificate creation or trust-store changes; not proof of DSP execution'
