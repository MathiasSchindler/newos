param(
    [string]$OutputDir = "experimental/snapdragon/models/translategemma-4b-it",
    [string]$Catalog = "experimental/snapdragon/tools/translate/translategemma-models.json",
    [string]$Python = "python",
    [switch]$AcceptGemmaLicense,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path

function Get-HuggingFaceToken {
    if ($env:HF_TOKEN) { return $env:HF_TOKEN.Trim() }
    if ($env:HUGGING_FACE_HUB_TOKEN) { return $env:HUGGING_FACE_HUB_TOKEN.Trim() }
    $tokenPaths = @()
    if ($env:HF_HOME) { $tokenPaths += (Join-Path $env:HF_HOME "token") }
    if ($env:USERPROFILE) {
        $tokenPaths += (Join-Path $env:USERPROFILE ".cache\huggingface\token")
    }
    foreach ($tokenPath in $tokenPaths) {
        if (Test-Path -LiteralPath $tokenPath -PathType Leaf) {
            $token = (Get-Content -LiteralPath $tokenPath -Raw).Trim()
            if ($token) { return $token }
        }
    }
    return $null
}

function Get-GitBlobSha1 {
    param([string]$Path)
    $length = (Get-Item -LiteralPath $Path).Length
    $prefix = [Text.Encoding]::ASCII.GetBytes("blob $length`0")
    $sha1 = [Security.Cryptography.SHA1]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        $null = $sha1.TransformBlock($prefix, 0, $prefix.Length, $prefix, 0)
        $buffer = New-Object byte[] (1024 * 1024)
        while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $null = $sha1.TransformBlock($buffer, 0, $read, $buffer, 0)
        }
        $null = $sha1.TransformFinalBlock((New-Object byte[] 0), 0, 0)
        return ([BitConverter]::ToString($sha1.Hash)).Replace("-", "").ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $sha1.Dispose()
    }
}

if (-not $AcceptGemmaLicense) {
    throw "Pass -AcceptGemmaLicense only after accepting the Gemma license for google/translategemma-4b-it."
}
$token = Get-HuggingFaceToken
if (-not $token) {
    throw "No Hugging Face credential found. Set HF_TOKEN or log in with the Hugging Face CLI."
}

Push-Location $repoRoot
try {
    $catalogPath = if ([IO.Path]::IsPathRooted($Catalog)) { $Catalog } else { Join-Path $repoRoot $Catalog }
    $catalogData = Get-Content -LiteralPath $catalogPath -Raw | ConvertFrom-Json
    $repository = $catalogData.model.repository
    $revision = $catalogData.model.revision
    if ($repository -ne "google/translategemma-4b-it" -or
        $revision -ne "10042cb0e6e7fdce748996a71dc3dc432a4e0c89" -or
        -not $catalogData.model.gated -or -not $catalogData.model.requires_license_acceptance) {
        throw "Catalog model identity or license policy is not canonical."
    }
    $authorization = "Authorization: Bearer $token"
    $metadataPath = Join-Path ([IO.Path]::GetTempPath()) ("translategemma-" + [Guid]::NewGuid() + ".json")
    try {
        & curl.exe --fail --location --silent --show-error --retry 5 --retry-all-errors `
            --header $authorization --output $metadataPath `
            "https://huggingface.co/api/models/$repository/revision/$revision"
        if ($LASTEXITCODE -ne 0) { throw "Authenticated model revision lookup failed." }
        $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
        if ($metadata.sha -ne $revision) { throw "Hugging Face resolved a different model revision." }
    } finally {
        Remove-Item -LiteralPath $metadataPath -Force -ErrorAction SilentlyContinue
    }

    $destination = if ([IO.Path]::IsPathRooted($OutputDir)) { $OutputDir } else { Join-Path $repoRoot $OutputDir }
    if ((Test-Path -LiteralPath $destination) -and -not $Force) {
        $existingLock = Join-Path $destination "source-lock.json"
        if (-not (Test-Path -LiteralPath $existingLock -PathType Leaf)) {
            throw "Destination exists without a source lock; pass -Force to replace it."
        }
        & $Python (Join-Path $PSScriptRoot "validate-translategemma.py") `
            --catalog $existingLock --model-dir $destination `
            --audit (Join-Path $destination "tensor-audit.json")
        if ($LASTEXITCODE -ne 0) { throw "Existing TranslateGemma directory failed validation." }
        Write-Output "Pinned TranslateGemma checkpoint is already staged at $destination"
        return
    }

    $stageDir = "$destination.partial"
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
    $baseUrl = "https://huggingface.co/$repository/resolve/$revision"
    for ($fileIndex = 0; $fileIndex -lt $catalogData.files.Count; ++$fileIndex) {
        $file = $catalogData.files[$fileIndex]
        $path = Join-Path $stageDir $file.name
        $partialPath = "$path.partial"
        $valid = $false
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $size = (Get-Item -LiteralPath $path).Length
            if ($size -eq [Int64]$file.size) {
                $actualSha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($file.sha256) {
                    $valid = $actualSha256 -eq $file.sha256.ToLowerInvariant()
                } elseif ($file.git_blob_sha1) {
                    $valid = (Get-GitBlobSha1 $path) -eq $file.git_blob_sha1.ToLowerInvariant()
                }
            }
            if (-not $valid) { Remove-Item -LiteralPath $path -Force }
        }
        if (-not $valid) {
            if ((Test-Path -LiteralPath $partialPath) -and
                (Get-Item -LiteralPath $partialPath).Length -ge [Int64]$file.size) {
                Remove-Item -LiteralPath $partialPath -Force
            }
            Write-Output "Downloading $($file.name) from $repository at $revision"
            & curl.exe --fail --location --silent --show-error --retry 5 --retry-all-errors `
                --continue-at - --header $authorization --output $partialPath `
                "$baseUrl/$($file.name)?download=true"
            if ($LASTEXITCODE -ne 0) { throw "Download failed for $($file.name)." }
            if ((Get-Item -LiteralPath $partialPath).Length -ne [Int64]$file.size) {
                throw "Size mismatch for $($file.name)."
            }
            $actualSha256 = (Get-FileHash -LiteralPath $partialPath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($file.sha256 -and $actualSha256 -ne $file.sha256.ToLowerInvariant()) {
                throw "SHA-256 mismatch for $($file.name)."
            }
            if (-not $file.sha256) {
                if (-not $file.git_blob_sha1 -or
                    (Get-GitBlobSha1 $partialPath) -ne $file.git_blob_sha1.ToLowerInvariant()) {
                    throw "Git blob identity mismatch for $($file.name)."
                }
                $file.sha256 = $actualSha256
            }
            Move-Item -LiteralPath $partialPath -Destination $path -Force
        } elseif (-not $file.sha256) {
            $file.sha256 = $actualSha256
        }
    }

    $lockPath = Join-Path $stageDir "source-lock.json"
    $lockPartial = "$lockPath.partial"
    [IO.File]::WriteAllText($lockPartial, ($catalogData | ConvertTo-Json -Depth 10) + "`n")
    Move-Item -LiteralPath $lockPartial -Destination $lockPath -Force
    Write-Output "Materialized SHA-256 source lock for all checkpoint files."
    $auditPath = Join-Path $stageDir "tensor-audit.json"
    & $Python (Join-Path $PSScriptRoot "validate-translategemma.py") `
        --catalog $lockPath --model-dir $stageDir --audit $auditPath
    if ($LASTEXITCODE -ne 0) { throw "TranslateGemma checkpoint validation failed." }

    $oldDir = "$destination.old"
    if (Test-Path -LiteralPath $oldDir) { Remove-Item -LiteralPath $oldDir -Recurse -Force }
    if (Test-Path -LiteralPath $destination) {
        Move-Item -LiteralPath $destination -Destination $oldDir
    }
    try {
        Move-Item -LiteralPath $stageDir -Destination $destination
    } catch {
        if (Test-Path -LiteralPath $oldDir) {
            Move-Item -LiteralPath $oldDir -Destination $destination
        }
        throw
    }
    if (Test-Path -LiteralPath $oldDir) { Remove-Item -LiteralPath $oldDir -Recurse -Force }
    Write-Output "Staged and validated $repository at $revision"
    Write-Output "  path: $destination"
    Write-Output "  audit: $(Join-Path $destination 'tensor-audit.json')"
} finally {
    $token = $null
    Pop-Location
}