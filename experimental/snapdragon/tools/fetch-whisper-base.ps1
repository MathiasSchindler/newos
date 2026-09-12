param(
    [string]$OutputDir = "experimental/snapdragon/models/whisper-base",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$model = "openai/whisper-base"
$revision = "e37978b90ca9030d5170a5c07aadb050351a65bb"
$modelSize = 290403936
$modelSha256 = "07cadb9f25677c8d50df603e66a98fbd842cce45047139baeb16e6219a1e807b"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$destination = Join-Path $repoRoot $OutputDir
$baseUrl = "https://huggingface.co/$model/resolve/$revision"
$files = @(
    "README.md",
    "config.json",
    "generation_config.json",
    "model.safetensors",
    "preprocessor_config.json",
    "tokenizer.json",
    "tokenizer_config.json",
    "special_tokens_map.json",
    "added_tokens.json",
    "vocab.json",
    "merges.txt"
)

New-Item -ItemType Directory -Force -Path $destination | Out-Null
foreach ($file in $files) {
    $path = Join-Path $destination $file
    if ($Force -or -not (Test-Path -LiteralPath $path)) {
        Write-Output "Downloading $model/$file at $revision"
        Invoke-WebRequest -Uri "$baseUrl/$file" -OutFile $path -UseBasicParsing
    }
}

$modelPath = Join-Path $destination "model.safetensors"
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
    $config.d_model -ne 512 -or
    $config.encoder_layers -ne 6 -or
    $config.encoder_attention_heads -ne 8 -or
    $config.encoder_ffn_dim -ne 2048 -or
    $config.decoder_layers -ne 6 -or
    $config.decoder_attention_heads -ne 8 -or
    $config.decoder_ffn_dim -ne 2048 -or
    $config.vocab_size -ne 51865 -or
    $config.max_target_positions -ne 448 -or
    $config.num_mel_bins -ne 80 -or
    $config.max_source_positions -ne 1500) {
    throw "Pinned checkpoint architecture does not match Whisper Base"
}

Write-Output "Staged $model"
Write-Output "  revision: $revision"
Write-Output "  license: Apache-2.0 (see README.md model card)"
Write-Output "  model: $modelSize bytes, SHA-256 $actualSha256"
Write-Output "  path: $destination"