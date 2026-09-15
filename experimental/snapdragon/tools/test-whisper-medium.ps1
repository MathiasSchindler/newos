param(
    [string]$ProbePath = 'experimental/snapdragon/build/medium-candidate/npu_probe.exe',
    [string]$ReferenceProbePath = 'experimental/snapdragon/build/npu_probe.exe',
    [string]$WavPath = 'experimental/snapdragon/data/long-form-35s.wav',
    [string]$OutputDirectory = 'experimental/snapdragon/data/medium-validation',
    [ValidateSet('cross,mlp', 'fused,logits', 'fused,self,logits')]
    [string[]]$MediumModes = @('cross,mlp', 'fused,logits')
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
Push-Location $repoRoot
try {
    foreach ($model in @('tiny', 'base', 'small', 'medium')) {
        $modes = if ($model -eq 'medium') { $MediumModes } else {
            @('cross,mlp', 'fused,self,logits')
        }
        foreach ($mode in $modes) {
            $modeName = $mode.Replace(',', '-')
            $candidateDirectory = Join-Path $OutputDirectory "$model-$modeName-candidate"
            & (Join-Path $PSScriptRoot 'profile-whisper.ps1') `
                -WavPath $WavPath -Model $model -DecoderOffload $mode `
                -ProbePath $ProbePath -OutputDirectory $candidateDirectory
            $candidate = Get-Content (Join-Path $candidateDirectory "$model-summary.json") `
                -Raw | ConvertFrom-Json
            $candidateOutput = Get-Content (Join-Path $candidateDirectory "$model.stdout.txt") -Raw
            $candidateErrors = Get-Content (Join-Path $candidateDirectory "$model.stderr.txt") -Raw
            if ($candidateOutput -match '<E>' -or $candidateErrors -match '<E>') {
                throw "$model $mode reported a QNN error"
            }
            if (-not $candidate.transcript_sha256 -or $candidate.generated_tokens -le 0 -or
                $candidate.windows -ne 2 -or $candidate.encoder_npu_ms -le 0) {
                throw "$model $mode did not produce two NPU-encoded transcription windows"
            }
            if ($mode -eq 'fused,self,logits' -and
                ($candidate.npu_fused_cross_mlp_graph_submissions -le 0 -or
                 $candidate.npu_self_attention_graph_submissions -le 0 -or
                 $candidate.npu_final_projection_graph_submissions -le 0)) {
                throw "$model did not execute all requested decoder offloads"
            }
            if ($mode -eq 'cross,mlp' -and
                ($candidate.npu_cross_attention_graph_submissions -le 0 -or
                 $candidate.npu_mlp_graph_submissions -le 0)) {
                throw "$model did not execute cross-attention and MLP offloads"
            }
            if ($mode -eq 'fused,logits' -and
                ($candidate.npu_fused_cross_mlp_graph_submissions -le 0 -or
                 $candidate.npu_final_projection_graph_submissions -le 0)) {
                throw "$model did not execute fused cross/MLP and logits offloads"
            }
            if ($model -ne 'medium') {
                $referenceDirectory = Join-Path $OutputDirectory "$model-$modeName-reference"
                & (Join-Path $PSScriptRoot 'profile-whisper.ps1') `
                    -WavPath $WavPath -Model $model -DecoderOffload $mode `
                    -ProbePath $ReferenceProbePath -OutputDirectory $referenceDirectory
                $reference = Get-Content (Join-Path $referenceDirectory "$model-summary.json") `
                    -Raw | ConvertFrom-Json
                if ($candidate.transcript_sha256 -ne $reference.transcript_sha256 -or
                    $candidate.generated_tokens -ne $reference.generated_tokens) {
                    throw "$model $mode transcript differs from the working binary"
                }
            }
            Write-Output "PASS $model $mode hardware transcription"
        }
    }
} finally {
    Pop-Location
}