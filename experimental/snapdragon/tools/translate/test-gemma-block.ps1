param(
	[string]$Binary = 'experimental/snapdragon/build/gemma-block/test-gemma-block.exe',
	[string]$WeightsDir = 'experimental/snapdragon/models/translategemma-4b-stage3',
	[string]$FixtureDir = 'experimental/snapdragon/models/translategemma-4b-stage6',
	[ValidateSet(4, 8)][int[]]$Bits = @(8, 4),
	[ValidateSet(0, 5)][int[]]$Layers = @(0, 5),
	[switch]$TestCleanup,
	[ValidateSet(0, 512, 1024, 2048)][int]$PromptBucket = 0,
	[switch]$RestorePrompt,
	[switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
$weights = Get-Content -LiteralPath "$WeightsDir/manifest.json" -Raw | ConvertFrom-Json
$fixtures = Get-Content -LiteralPath "$FixtureDir/manifest.json" -Raw | ConvertFrom-Json
$purpose = if ($PromptBucket) { 'stage7-prompt' } else { 'stage6-block-cache' }
if ($fixtures.schema_version -ne 2 -or $fixtures.purpose -ne $purpose -or
	$fixtures.execution_contract.global_rope.factor -ne 8 -or
	$fixtures.execution_contract.global_rope.theta -ne 1000000 -or
	$fixtures.execution_contract.global_rope.type -ne 'linear' -or
	$fixtures.execution_contract.local_rope.factor -ne 1 -or
	$fixtures.execution_contract.local_rope.theta -ne 10000 -or
	$fixtures.execution_contract.local_rope.type -ne 'default' -or
	$fixtures.execution_contract.residual.quantized_divisor -ne 32 -or
	$fixtures.execution_contract.residual.post_norm_gain_divisor -ne 32 -or
	$fixtures.execution_contract.residual.pre_norm_epsilon -ne 9.765625e-10 -or
	$fixtures.execution_contract.residual.storage -ne 'activation-dtype' -or
	$fixtures.weights_manifest_sha256 -ne (Get-FileHash "$WeightsDir/manifest.json" -Algorithm SHA256).Hash.ToLowerInvariant()) {
	throw 'Block fixture contract or weight manifest mismatch'
}
$binaryPath = (Resolve-Path $Binary).Path
$outputDir = Split-Path -Parent $binaryPath
$selectedLayers = $Layers
if ($PromptBucket) {
	if (-not $PrepareOnly) { $Bits = @(4) }
	if ($Bits.Count -ne 1) { throw 'Prompt binding preparation requires one precision' }
	$selectedLayers = @(34)
}
foreach ($bit in $Bits) {
	$variant = 'w{0}a16' -f $bit
	foreach ($layer in $selectedLayers) {
		$entries = @()
		foreach ($entry in $weights.variants.$variant.artifacts) {
			if ($PromptBucket -or $entry.name.StartsWith(('language_model.model.layers.{0}.' -f $layer))) {
				$entries += @{ Name = $entry.name; Path = (Resolve-Path (Join-Path "$WeightsDir/$variant" $entry.path)).Path }
			}
		}
		foreach ($entry in $fixtures.artifacts) {
			if ($PrepareOnly -and $PromptBucket -and $entry.name -notin @('fixture/prompt/local-cos','fixture/prompt/local-sin','fixture/prompt/global-cos','fixture/prompt/global-sin')) { continue }
			if (($PromptBucket -and $entry.name.StartsWith('fixture/prompt/')) -or $entry.name.StartsWith(('fixture/{0}/layer-{1}/' -f $variant, $layer))) {
				$entries += @{ Name = $entry.name; Path = (Resolve-Path (Join-Path $FixtureDir $entry.path)).Path }
			}
		}
		$bindingPath = Join-Path $outputDir ('{0}-layer-{1}.gmb' -f $variant, $layer)
		if ($PromptBucket) { $bindingPath = Join-Path $outputDir ('prompt-{0}.gmb' -f $PromptBucket) }
		$stream = [IO.File]::Create($bindingPath)
		$writer = New-Object IO.BinaryWriter($stream)
		try {
			$writer.Write([uint32]0x36424d47); $writer.Write([uint32]2)
			$writer.Write([uint32]$layer); $writer.Write([uint32]$bit); $writer.Write([uint32]$entries.Count)
			foreach ($entry in $entries) {
				$nameBytes = [Text.Encoding]::ASCII.GetBytes($entry.Name)
				$pathBytes = [Text.Encoding]::ASCII.GetBytes($entry.Path)
				if ([Text.Encoding]::ASCII.GetString($pathBytes) -cne $entry.Path) { throw 'Runner paths must be ASCII' }
				$writer.Write([uint32]$nameBytes.Length); $writer.Write([uint32]$pathBytes.Length)
				$writer.Write($nameBytes); $writer.Write($pathBytes)
			}
		} finally { $writer.Dispose(); $stream.Dispose() }
		if ($PrepareOnly) { Write-Output ('Prepared {0} W{1} binding entries: {2}' -f $entries.Count,$bit,$bindingPath); continue }
		$logPath = Join-Path $outputDir ('{0}-layer-{1}.log' -f $variant, $layer)
		$runnerArguments = @($bindingPath)
		if ($PromptBucket) {
			$mode = if ($RestorePrompt) { 'restore' } else { 'prompt' }
			$runnerArguments += '{0}-{1}' -f $mode, $PromptBucket
			$logPath = Join-Path $outputDir ('prompt-{0}.log' -f $PromptBucket)
		}
		$ErrorActionPreference = 'Continue'
		if ($PromptBucket) {
			& $binaryPath $bindingPath envelope-regression 2>&1 | ForEach-Object { $_.ToString() } |
				Tee-Object -FilePath (Join-Path $outputDir 'envelope-regression.log')
			if ($LASTEXITCODE -ne 0) { throw 'Prompt envelope regression failed' }
			& $binaryPath $bindingPath position-regression 2>&1 | ForEach-Object { $_.ToString() } |
				Tee-Object -FilePath (Join-Path $outputDir 'position-regression.log')
			$regressionCode = $LASTEXITCODE
			if ($regressionCode -ne 0) { throw ('Position regression failed: exit {0}' -f $regressionCode) }
		}
		& $binaryPath @runnerArguments 2>&1 | ForEach-Object { $_.ToString() } | Tee-Object -FilePath $logPath
		$exitCode = $LASTEXITCODE
		$ErrorActionPreference = 'Stop'
		if ($exitCode -ne 0) { throw ('Block gate failed: {0} layer {1}, exit {2}' -f $variant, $layer, $exitCode) }
		if ($PromptBucket) {
			$invalidBinding = Join-Path $outputDir ('invalid-context-' + [Guid]::NewGuid().ToString('N') + '.gmb')
			$invalidContext = $invalidBinding + '.context'
			try {
				Copy-Item -LiteralPath $bindingPath -Destination $invalidBinding
				$source = [IO.File]::OpenRead($bindingPath + '.context')
				try {
					$reader = New-Object IO.BinaryReader($source)
					$sample = $reader.ReadBytes(65536)
				} finally { $source.Dispose() }
				foreach ($fileBytes in 0, 16, 65536) {
					$target = [IO.File]::Create($invalidContext)
					try { $target.Write($sample, 0, $fileBytes) } finally { $target.Dispose() }
					$ErrorActionPreference = 'Continue'
					$rejection = & $binaryPath $invalidBinding ('restore-{0}' -f $PromptBucket) 2>&1
					$rejectionCode = $LASTEXITCODE
					$ErrorActionPreference = 'Stop'
					$rejectionText = $rejection | Out-String
					$rejectionText | Set-Content -LiteralPath (Join-Path $outputDir ('reject-{0}-{1}.log' -f $PromptBucket, $fileBytes))
					if ($rejectionCode -ne 1 -or $rejectionText -notmatch 'cleanup errors: 0' -or
						$rejectionText -match 'prompt restore:|PASS prompt restore') {
						throw ('Context file rejection failed: bucket {0}, length {1}' -f $PromptBucket, $fileBytes)
					}
					Write-Output ('PASS context file rejection: bucket {0}, length {1}' -f $PromptBucket, $fileBytes)
				}
			} finally {
				Remove-Item -LiteralPath $invalidBinding, $invalidContext -Force -ErrorAction SilentlyContinue
			}
		}
		if ($TestCleanup -and $bit -eq $Bits[0] -and $layer -eq $Layers[0]) {
			foreach ($point in 'context', 'graph', 'finalized', 'registered', 'executed') {
				$ErrorActionPreference = 'Continue'
				$failureOutput = & $binaryPath $bindingPath $point 2>&1
				$failureCode = $LASTEXITCODE
				$ErrorActionPreference = 'Stop'
				$failureText = $failureOutput | Out-String
				$failureText | Set-Content -LiteralPath (Join-Path $outputDir ('cleanup-{0}.log' -f $point))
				if ($failureCode -ne 1 -or $failureText -notmatch ('Injected failure: ' + $point) -or
					$failureText -notmatch 'cleanup errors: 0' -or $failureText -match 'PASS block intermediates') {
					throw ('Cleanup gate failed at {0}: {1}' -f $point, $failureText)
				}
				Write-Output ('PASS failure cleanup after {0}' -f $point)
			}
		}
	}
}
