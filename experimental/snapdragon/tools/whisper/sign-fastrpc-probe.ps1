param(
    [ValidateSet('Prepare', 'Verify', 'Trust', 'Test')]
    [string]$Action = 'Verify',
    [string]$ExpectedThumbprint = ''
)

$ErrorActionPreference = 'Stop'
if ($Action -eq 'Trust' -and $ExpectedThumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
    throw 'Trust requires the explicitly reviewed 40-digit ExpectedThumbprint'
}
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$directory = Join-Path $root 'data/fastrpc-signing'
$payload = Join-Path $directory 'payload'
$module = Join-Path $payload 'fastrpc_probe_skel.so'
$catalog = Join-Path $directory 'fastrpc_probe_skel.cat'
$publicKey = Join-Path $directory 'development.cer'
$receipt = Join-Path $directory 'receipt.json'
$subject = 'CN=newos FastRPC Probe Development'

Add-Type -AssemblyName System.Security
if ($Action -eq 'Prepare') {
    $source = Join-Path $root 'build/fastrpc-probe/fastrpc_probe_skel.so'
    if (-not (Test-Path -LiteralPath $source)) { throw 'Build the FastRPC DSP module first' }
    New-Item -ItemType Directory -Force -Path $payload | Out-Null
    $extraFiles = @(Get-ChildItem -LiteralPath $payload -Force | Where-Object { $_.Name -ne 'fastrpc_probe_skel.so' })
    if ($extraFiles.Count) { throw 'Signing payload directory must contain only the probe module' }
    Copy-Item -LiteralPath $source -Destination $module -Force
    New-FileCatalog -Path $payload -CatalogFilePath $catalog -CatalogVersion 2 | Out-Null
    if (Test-Path -LiteralPath $publicKey) {
        $publicCertificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2($publicKey)
        $certificate = Get-Item ('Cert:/CurrentUser/My/' + $publicCertificate.Thumbprint)
    } else {
        $existing = @(Get-ChildItem Cert:/CurrentUser/My -CodeSigningCert | Where-Object { $_.Subject -eq $subject })
        if ($existing.Count -gt 1) { throw 'Multiple probe certificates exist; select the intended identity explicitly' }
        if ($existing.Count -eq 1) {
            $certificate = $existing[0]
        } else {
            $certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
                -FriendlyName 'newos FastRPC probe development only' -CertStoreLocation Cert:/CurrentUser/My `
                -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable `
                -Provider 'Microsoft Software Key Storage Provider' -NotAfter (Get-Date).AddDays(90)
        }
        Export-Certificate -Cert $certificate -FilePath $publicKey -Type CERT | Out-Null
    }
    if ($certificate.Subject -ne $subject -or -not $certificate.HasPrivateKey -or $certificate.NotAfter -le (Get-Date)) {
        throw 'Probe signing certificate is not usable'
    }
    $signature = Set-AuthenticodeSignature -LiteralPath $catalog -Certificate $certificate -HashAlgorithm SHA256 -IncludeChain All
    if (-not $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
        throw 'Catalog was not signed by the probe certificate'
    }
    [ordered]@{
        schema = 1
        createdUtc = [DateTime]::UtcNow.ToString('o')
        certificateThumbprint = $certificate.Thumbprint
        certificateSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $publicKey).Hash
        moduleSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $module).Hash
        catalogSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $catalog).Hash
    } | ConvertTo-Json | Set-Content -Encoding ASCII -LiteralPath $receipt
}

$record = Get-Content -Raw -LiteralPath $receipt | ConvertFrom-Json
if ($record.schema -ne 1) { throw 'Unsupported signing receipt' }
foreach ($entry in @(
    @{ Path = $module; Hash = $record.moduleSha256 },
    @{ Path = $catalog; Hash = $record.catalogSha256 },
    @{ Path = $publicKey; Hash = $record.certificateSha256 }
)) {
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $entry.Path).Hash -ne $entry.Hash) {
        throw ('Artifact differs from signing receipt: ' + $entry.Path)
    }
}
$publicCertificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2($publicKey)
if ($publicCertificate.Subject -ne $subject -or $publicCertificate.Thumbprint -ne $record.certificateThumbprint -or $publicCertificate.HasPrivateKey) {
    throw 'Unexpected public certificate identity or private key material'
}
$cms = New-Object Security.Cryptography.Pkcs.SignedCms
$cms.Decode([IO.File]::ReadAllBytes($catalog))
if ($cms.SignerInfos.Count -ne 1 -or $cms.SignerInfos[0].Certificate.Thumbprint -ne $record.certificateThumbprint) {
    throw 'Unexpected catalog signer'
}
$cms.CheckSignature($true)
if ((Test-FileCatalog -Path $payload -CatalogFilePath $catalog) -ne 'Valid') {
    throw 'Catalog does not match the probe payload'
}
if ($Action -eq 'Trust') {
    if ($publicCertificate.Thumbprint -ne $ExpectedThumbprint) { throw 'Reviewed certificate thumbprint does not match' }
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run the Trust action yourself in PowerShell as Administrator; this script never requests elevation'
    }
    foreach ($storeName in @('Root', 'TrustedPublisher')) {
        $store = New-Object Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
        try {
            $store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
            $store.Add($publicCertificate)
        } finally {
            $store.Close()
        }
    }
}
$signature = Get-AuthenticodeSignature -LiteralPath $catalog
if ($Action -eq 'Trust' -and $signature.Status -ne 'Valid') {
    throw ('Certificate import completed but catalog trust verification failed: ' + $signature.StatusMessage)
}
Write-Output ('CertificateThumbprint=' + $record.certificateThumbprint)
Write-Output ('ModuleSHA256=' + $record.moduleSha256)
Write-Output ('CatalogSHA256=' + $record.catalogSha256)
Write-Output ('PublicCertificate=' + $publicKey)
Write-Output ('AuthenticodeTrustStatus=' + $signature.Status)
Write-Output ('LocalMachineRootPresent=' + (Test-Path ('Cert:/LocalMachine/Root/' + $record.certificateThumbprint)))
Write-Output ('LocalMachinePublisherPresent=' + (Test-Path ('Cert:/LocalMachine/TrustedPublisher/' + $record.certificateThumbprint)))
Write-Output 'CATALOG_SIGNATURE_AND_MEMBERSHIP_PASS (not proof of driver acceptance)'

if ($Action -eq 'Test') {
    if ($signature.Status -ne 'Valid') { throw 'Hardware tests require a trusted catalog first' }
    $hostProbe = Join-Path $root 'build/fastrpc-probe/fastrpc_probe.exe'
    $drivers = @(Get-ChildItem "$env:SystemRoot/System32/DriverStore/FileRepository/qcnspmcdm8380.inf_arm64_*/libcdsprpc.dll")
    if ($drivers.Count -ne 1) { throw 'Expected exactly one MCDM FastRPC driver; select the intended driver explicitly' }
    $evidence = Join-Path $root ('data/fastrpc-tested-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $evidence | Out-Null
    Copy-Item -LiteralPath $receipt -Destination (Join-Path $evidence 'signing-receipt.json')
    $results = @()
    $cases = @(
        @{ Name = 'host'; Invoke = $false; Exit = 0 },
        @{ Name = 'signed-first'; Invoke = $true; Exit = 0 },
        @{ Name = 'signed-repeat'; Invoke = $true; Exit = 0 },
        @{ Name = 'missing-catalog'; Invoke = $true; Exit = 9 },
        @{ Name = 'missing-module'; Invoke = $true; Exit = 9 },
        @{ Name = 'modified-module'; Invoke = $true; Exit = 9 },
        @{ Name = 'signed-after-controls'; Invoke = $true; Exit = 0 }
    )
    foreach ($case in $cases) {
        $caseDirectory = Join-Path $evidence $case.Name
        New-Item -ItemType Directory -Path $caseDirectory | Out-Null
        Copy-Item -LiteralPath $hostProbe -Destination $caseDirectory
        if ($case.Name -ne 'missing-module') { Copy-Item -LiteralPath $module -Destination $caseDirectory }
        if ($case.Name -ne 'missing-catalog') { Copy-Item -LiteralPath $catalog -Destination $caseDirectory }
        if ($case.Name -eq 'modified-module') {
            $original = [IO.File]::ReadAllBytes($module)
            $modified = New-Object byte[] ($original.Length + 1)
            [Array]::Copy($original, $modified, $original.Length)
            $modified[$original.Length] = 0x5a
            [IO.File]::WriteAllBytes((Join-Path $caseDirectory 'fastrpc_probe_skel.so'), $modified)
        }
        $hashes = @(Get-ChildItem -LiteralPath $caseDirectory -File | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash)
        $hashes | ConvertTo-Json | Set-Content -Encoding ASCII -LiteralPath (Join-Path $caseDirectory 'hashes.json')
        $info = New-Object Diagnostics.ProcessStartInfo
        $info.FileName = Join-Path $caseDirectory 'fastrpc_probe.exe'
        $info.WorkingDirectory = $caseDirectory
        $info.Arguments = '"' + $drivers[0].FullName + '"'
        if ($case.Invoke) { $info.Arguments += ' --invoke' }
        $info.UseShellExecute = $false
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.EnvironmentVariables['ADSP_LIBRARY_PATH'] = $caseDirectory
        $info.EnvironmentVariables['PATH'] = "$env:SystemRoot/System32;$env:SystemRoot"
        $process = New-Object Diagnostics.Process
        $process.StartInfo = $info
        try {
            $null = $process.Start()
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            $timedOut = -not $process.WaitForExit(30000)
            if ($timedOut) { $process.Kill(); $process.WaitForExit() }
            $output = $stdout.Result + $stderr.Result
            $output | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $caseDirectory 'probe.log')
            $valid = -not $timedOut -and $process.ExitCode -eq $case.Exit
            if ($case.Invoke -and $case.Exit -eq 0) {
                $valid = $valid -and [regex]::Matches($output, 'remote_invoke.mismatches=0').Count -eq 3 -and
                    $output -match 'remote_open.status=0' -and $output -match 'remote_close.status=0' -and
                    $output -match 'session_close=handled_by_last_handle' -and $output -match 'custom_dsp_execution=verified'
            } elseif ($case.Invoke) {
                $valid = $valid -and $output -match 'custom_dsp_execution=failed' -and $output -match 'session_close.status=0'
            } else {
                $valid = $valid -and $output -match 'custom_dsp_execution=not_tested'
            }
            $results += [pscustomobject]@{ name = $case.Name; exit = $process.ExitCode; timeout = $timedOut; passed = $valid }
            [ordered]@{
                driver = $drivers[0].FullName
                driverSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $drivers[0].FullName).Hash
                cases = $results
                complete = ($results.Count -eq $cases.Count -and $valid)
            } | ConvertTo-Json -Depth 4 | Set-Content -Encoding ASCII -LiteralPath (Join-Path $evidence 'results.json')
            Write-Output ('{0}: exit={1}, passed={2}' -f $case.Name, $process.ExitCode, $valid)
            if (-not $valid) { throw ('FastRPC check failed: ' + $case.Name + '; evidence: ' + $caseDirectory) }
        } finally {
            $process.Dispose()
        }
    }
    Write-Output ('EVIDENCE=' + $evidence)
    Write-Output 'CUSTOM_DSP_EXECUTION_AND_NEGATIVE_CONTROLS_PASS'
}