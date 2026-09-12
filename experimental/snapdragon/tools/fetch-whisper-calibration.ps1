param(
    [string]$OutputDir = "experimental/snapdragon/models/calibration/fleurs",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$dataset = "google/fleurs"
$revision = "70bb2e84b976b7e960aa89f1c648e09c59f894dd"
$configs = @(
    "ar_eg",
    "ca_es",
    "cmn_hans_cn",
    "de_de",
    "en_us",
    "fr_fr",
    "he_il",
    "hi_in",
    "ja_jp",
    "ko_kr",
    "pt_br",
    "ru_ru",
    "sw_ke",
    "ta_in",
    "vi_vn",
    "yue_hant_hk"
)
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$destination = Join-Path $repoRoot $OutputDir

$datasetInfo = Invoke-RestMethod -Uri "https://huggingface.co/api/datasets/$dataset"
if ($datasetInfo.sha -ne $revision) {
    throw "$dataset current revision is $($datasetInfo.sha); expected pinned revision $revision"
}
if ($datasetInfo.cardData.license -ne "cc-by-4.0") {
    throw "$dataset license is $($datasetInfo.cardData.license); expected cc-by-4.0"
}

New-Item -ItemType Directory -Force -Path $destination | Out-Null
$readmePath = Join-Path $destination "FLEURS-README.md"
if ($Force -or -not (Test-Path -LiteralPath $readmePath)) {
    Invoke-WebRequest -Uri "https://huggingface.co/datasets/$dataset/resolve/$revision/README.md" -OutFile $readmePath -UseBasicParsing
}

$clips = @()
foreach ($config in $configs) {
    $query = "https://datasets-server.huggingface.co/first-rows?dataset=google%2Ffleurs&config=$config&split=validation"
    $response = Invoke-RestMethod -Uri $query
    if ($response.rows.Count -lt 1) {
        throw "No validation rows returned for FLEURS $config"
    }
    $record = $response.rows[0]
    $audio = $record.row.audio[0]
    if ($audio.type -ne "audio/wav" -or $audio.src -notmatch "/--/$revision/--/") {
        throw "FLEURS $config row 0 did not resolve to pinned WAV revision $revision"
    }
    $fileName = "$config-$($record.row.id).wav"
    $path = Join-Path $destination $fileName
    if ($Force -or -not (Test-Path -LiteralPath $path)) {
        Write-Output "Downloading FLEURS $config validation row 0"
        Invoke-WebRequest -Uri $audio.src -OutFile $path -UseBasicParsing
    }
    $clips += [ordered]@{
        config = $config
        split = "validation"
        row = 0
        id = $record.row.id
        language = $record.row.language
        gender = $record.row.gender
        sample_rate = 16000
        sample_format = "IEEE_FLOAT_32LE"
        bits_per_sample = 32
        samples = $record.row.num_samples
        transcript = $record.row.transcription
        file = $fileName
        size = (Get-Item -LiteralPath $path).Length
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        source_asset = $audio.src.Split('?')[0]
    }
}

$manifest = [ordered]@{
    format = "newos.whisper_calibration_audio.v1"
    dataset = $dataset
    revision = $revision
    license = "CC-BY-4.0"
    source = "https://huggingface.co/datasets/$dataset/tree/$revision"
    selection = "validation row 0 from each listed language configuration"
    clips = $clips
}
$utf8 = New-Object System.Text.UTF8Encoding($false)
$manifestJson = $manifest | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText((Join-Path $destination "manifest.json"), $manifestJson, $utf8)
Write-Output "Staged $($clips.Count) FLEURS clips at $destination"