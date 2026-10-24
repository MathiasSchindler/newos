param(
    [string]$StageDirectory = '',
    [string]$DriverPath = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$build = Join-Path $repo 'experimental\snapdragon\build\nemotron-fastrpc-fp32'
if (-not $StageDirectory -or -not [IO.Path]::IsPathRooted($StageDirectory)) { throw 'An absolute new StageDirectory is required' }
if ((Test-Path -LiteralPath $StageDirectory) -and @(Get-ChildItem -LiteralPath $StageDirectory -Force).Count) {
    throw 'Refusing to overwrite an existing signed snapshot'
}
$public = Join-Path $repo 'experimental\snapdragon\data\fastrpc-signing\development.cer'
$certificateHash = '2953364187cf4d6758961596aca6441c89c9aae4f30a2eb6bf11b3c7d36689fe'
$sourceReceipt = Get-Content -Raw -LiteralPath "$build\source-receipt.json" | ConvertFrom-Json
foreach ($entry in $sourceReceipt.sources.PSObject.Properties) {
    if ((Get-FileHash -LiteralPath $entry.Value.file -Algorithm SHA256).Hash -ine $entry.Value.sha256) {
        throw "Source differs from the completed build; refusing signing: $($entry.Value.file)"
    }
}
if ((Get-FileHash -LiteralPath $public -Algorithm SHA256).Hash -ine $certificateHash) {
    throw 'Existing development certificate fingerprint mismatch'
}
if (-not $DriverPath) {
    $candidates = @(Get-ChildItem "$env:SystemRoot\System32\DriverStore\FileRepository\qcnspmcdm8380.inf_arm64_*\libcdsprpc.dll")
    if ($candidates.Count -ne 1) { throw 'Select an explicit DriverPath; exactly one approved OEM driver was not found' }
    $DriverPath = $candidates[0].FullName
}
$DriverPath = (Resolve-Path -LiteralPath $DriverPath).Path
Add-Type -AssemblyName System.Security
$identity = New-Object Security.Cryptography.X509Certificates.X509Certificate2($public)
$certificate = Get-Item ('Cert:\CurrentUser\My\' + $identity.Thumbprint)
if (-not $certificate.HasPrivateKey -or $certificate.NotAfter -le (Get-Date)) { throw 'Existing signing identity unusable' }
New-Item -ItemType Directory -Force -Path "$StageDirectory\payload" | Out-Null
if (@(Get-ChildItem -LiteralPath "$StageDirectory\payload" -Force | Where-Object {$_.Name -ne 'nm_fastrpc_skel.so'}).Count) {
    throw 'Unexpected signing payload files'
}
$payload = Join-Path $StageDirectory 'payload\nm_fastrpc_skel.so'
$dsp = Join-Path $StageDirectory 'nm_fastrpc_skel.so'
$hostExe = Join-Path $StageDirectory 'nemotron-fastrpc-fp32.exe'
$catalog = Join-Path $StageDirectory 'nm_fastrpc_skel.cat'
Copy-Item -LiteralPath "$build\nm_fastrpc_skel.so" -Destination $payload -Force
Copy-Item -LiteralPath "$build\nemotron-fastrpc-fp32.exe" -Destination $hostExe -Force
New-FileCatalog -Path "$StageDirectory\payload" -CatalogFilePath $catalog -CatalogVersion 2 | Out-Null
$signature = Set-AuthenticodeSignature -LiteralPath $catalog -Certificate $certificate -HashAlgorithm SHA256 -IncludeChain All
if (-not $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $identity.Thumbprint) { throw 'Wrong catalog signer' }
$cms = New-Object Security.Cryptography.Pkcs.SignedCms
$cms.Decode([IO.File]::ReadAllBytes($catalog)); $cms.CheckSignature($true)
if ($cms.SignerInfos.Count -ne 1 -or $cms.SignerInfos[0].Certificate.Thumbprint -ne $identity.Thumbprint -or
    (Test-FileCatalog -Path "$StageDirectory\payload" -CatalogFilePath $catalog) -ne 'Valid') { throw 'Catalog membership/signature failed' }
Copy-Item -LiteralPath $payload -Destination $dsp -Force
$snapshotSources = [ordered]@{}
foreach ($entry in $sourceReceipt.sources.PSObject.Properties) {
    $original = [IO.Path]::GetFullPath($entry.Value.file)
    if (-not $original.StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Only project-owned sources may be copied into the snapshot'
    }
    $relative = $original.Substring($repo.Length + 1)
    $destination = Join-Path $StageDirectory ('sources\' + $relative)
    New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath $original -Destination $destination
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ine $entry.Value.sha256) {
        throw "Concurrent source edit detected while snapshotting: $original"
    }
    $snapshotSources[$entry.Name] = @{file=$destination;source_file=$original;sha256=$entry.Value.sha256}
}
[ordered]@{
    schema_version = 1
    backend = 'OEMFastRPC-HVX-FP32'
    precision = 32
    qnnCompiler = $false
    hostModelKernels = $false
    moduleEntry = 'nm_fastrpc_skel_invoke'
    hvxBytes = 128
    protocol_version = 8
    coefficient_staging = '128-byte aligned immutable tensors by default; NM_FP32_RPC_ALIGN_COEFFICIENTS=0 selects raw slices'
    kernel_timing = 'maximum per-worker nm_fp32_execute duration; excludes dispatcher/barrier administration'
    metadata_versions = @(1, 2)
    packed_weight_columns = 32
    worker_profiles = @(1, 4)
    power_profiles = @('baseline', 'scoped DCVS_V3 core/bus TURBO latency40us; destroy client every invoke')
    hvx_output_vector_counter = $sourceReceipt.hvxProfile
    sources = $snapshotSources
    dsp = @{file=$dsp;sha256=(Get-FileHash -LiteralPath $dsp -Algorithm SHA256).Hash.ToLowerInvariant();
        catalog_file=$catalog;catalog_sha256=(Get-FileHash -LiteralPath $catalog -Algorithm SHA256).Hash.ToLowerInvariant()}
    host = @{file=$hostExe;sha256=(Get-FileHash -LiteralPath $hostExe -Algorithm SHA256).Hash.ToLowerInvariant()}
    driver = @{file=$DriverPath;sha256=(Get-FileHash -LiteralPath $DriverPath -Algorithm SHA256).Hash.ToLowerInvariant()}
    certificateThumbprint = $identity.Thumbprint
    certificateSha256 = $certificateHash
} | ConvertTo-Json -Depth 4 | Set-Content -Encoding ASCII -LiteralPath "$StageDirectory\manifest.json"
Write-Output "SIGNED_CATALOG_MEMBERSHIP_PASS $StageDirectory (not execution proof; existing identity only)"
