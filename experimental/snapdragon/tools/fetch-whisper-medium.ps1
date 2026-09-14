param(
    [string]$OutputDir = "experimental/snapdragon/models/whisper-medium",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$catalog = Get-Content (Join-Path $PSScriptRoot "whisper-models.json") -Raw | ConvertFrom-Json
$model = $catalog.medium.repository
$revision = $catalog.medium.revision
$modelSize = 3055544304L
$modelSha256 = $catalog.medium.sha256
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$destination = Join-Path $repoRoot $OutputDir
$baseUrl = "https://huggingface.co/$model/resolve/$revision"
$files = @(
    "README.md", "config.json", "generation_config.json", "model.safetensors",
    "preprocessor_config.json", "tokenizer.json", "tokenizer_config.json",
    "special_tokens_map.json", "added_tokens.json", "vocab.json", "merges.txt"
)

New-Item -ItemType Directory -Force -Path $destination | Out-Null
$modelPath = Join-Path $destination "model.safetensors"
if ((Test-Path -LiteralPath $modelPath) -and
    (Get-Item -LiteralPath $modelPath).Length -ne $modelSize) {
    Move-Item -LiteralPath $modelPath -Destination "$modelPath.partial" -Force
}
foreach ($file in $files) {
    $path = Join-Path $destination $file
    $partialPath = "$path.partial"
    if ($Force) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $partialPath -Force -ErrorAction SilentlyContinue
    }
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Output "Downloading $model/$file at $revision"
        & curl.exe --fail --location --retry 5 --retry-all-errors `
            --continue-at - --output $partialPath "$baseUrl/$file"
        if ($LASTEXITCODE -ne 0) { throw "Failed to download $model/$file" }
        Move-Item -LiteralPath $partialPath -Destination $path -Force
    }
}

$actualSize = (Get-Item -LiteralPath $modelPath).Length
if ($actualSize -ne $modelSize) {
    throw "model.safetensors size is $actualSize; expected $modelSize"
}
$actualSha256 = (Get-FileHash -LiteralPath $modelPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualSha256 -ne $modelSha256) {
    throw "model.safetensors SHA-256 is $actualSha256; expected $modelSha256"
}
$config = Get-Content -LiteralPath (Join-Path $destination "config.json") -Raw | ConvertFrom-Json
if ($config.model_type -ne "whisper" -or
    $config.d_model -ne 1024 -or
    $config.encoder_layers -ne 24 -or
    $config.encoder_attention_heads -ne 16 -or
    $config.encoder_ffn_dim -ne 4096 -or
    $config.decoder_layers -ne 24 -or
    $config.decoder_attention_heads -ne 16 -or
    $config.decoder_ffn_dim -ne 4096 -or
    $config.vocab_size -ne 51865 -or
    $config.max_target_positions -ne 448 -or
    $config.num_mel_bins -ne 80 -or
    $config.max_source_positions -ne 1500) {
    throw "Pinned checkpoint architecture does not match Whisper Medium"
}
Write-Output "Staged $model"
Write-Output "  revision: $revision"
Write-Output "  license: Apache-2.0 (see README.md model card)"
Write-Output "  model: $modelSize bytes, SHA-256 $actualSha256"
Write-Output "  path: $destination"