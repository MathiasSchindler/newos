param(
    [switch]$IncludePowerState,
    [switch]$CheckLayout,
    [string]$Python = 'experimental/snapdragon/build/calibration-venv/Scripts/python.exe'
)

$ErrorActionPreference = "Stop"

if ($CheckLayout) {
    $root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
    $repo = (Resolve-Path (Join-Path $root '../..')).Path
    foreach ($directory in 'src/apps/whisper','src/apps/ocr','src/apps/translate','src/tools/probe','src/shared','tools/whisper','tools/ocr','tools/translate','tools/shared','docs/apps','docs/platform') {
        if (-not (Test-Path (Join-Path $root $directory) -PathType Container)) { throw ('Missing directory: ' + $directory) }
    }
    foreach ($directory in 'src/tools/whisper','src/tools/ocr','src/tools/gemma') {
        if (Test-Path (Join-Path $root $directory)) { throw ('Obsolete source directory: ' + $directory) }
    }
    if (@(Get-ChildItem (Join-Path $root 'tools') -File).Count) { throw 'Developer files must belong to a tools group' }
    $scripts = @(Get-Item (Join-Path $root 'make.ps1')) + @(Get-ChildItem (Join-Path $root 'tools') -Recurse -File -Filter '*.ps1')
    foreach ($script in $scripts) {
        $tokens = $null
        $parseErrors = $null
        $null = [Management.Automation.Language.Parser]::ParseFile($script.FullName, [ref]$tokens, [ref]$parseErrors)
        if ($parseErrors.Count) { throw ($script.FullName + ': ' + ($parseErrors -join '; ')) }
    }
    $sources = @(Get-ChildItem (Join-Path $root 'src') -Recurse -File | Where-Object { $_.Extension -in '.c','.h' })
    foreach ($source in $sources) {
        $text = [IO.File]::ReadAllText($source.FullName)
        foreach ($include in [regex]::Matches($text, '(?m)^\s*#include\s+"([^"]+)"')) {
            $name = $include.Groups[1].Value
            $found = $false
            foreach ($directory in @($source.DirectoryName, (Join-Path $repo 'src/shared'), (Join-Path $root 'src/shared'), (Join-Path $root 'src/apps/translate'), (Join-Path $root 'src/apps/whisper'), (Join-Path $root 'src/apps/ocr'), (Join-Path $root 'src/tools/probe'))) {
                if (Test-Path (Join-Path $directory $name) -PathType Leaf) { $found = $true; break }
            }
            if (-not $found) { throw ('Missing include in {0}: {1}' -f $source.FullName,$name) }
        }
    }
    foreach ($catalog in (Get-ChildItem (Join-Path $root 'tools') -Recurse -Filter '*.json' -File)) {
        $null = ConvertFrom-Json ([IO.File]::ReadAllText($catalog.FullName))
    }
    $documents = @(Get-Item (Join-Path $root 'README.md')) + @(Get-ChildItem (Join-Path $root 'docs') -Recurse -Filter '*.md' -File)
    foreach ($document in $documents) {
        foreach ($link in [regex]::Matches([IO.File]::ReadAllText($document.FullName), '\]\(([^\s)#]+)(?:#[^)]*)?\)')) {
            $target = $link.Groups[1].Value
            if ($target -match '^[a-z]+:|^/') { continue }
            $target = [Uri]::UnescapeDataString($target)
            if (-not (Test-Path (Join-Path $document.DirectoryName $target))) { throw ('Missing documentation link in {0}: {1}' -f $document.FullName,$target) }
        }
    }
    $taskFile = Join-Path $repo '.vscode/tasks.json'
    if (Test-Path $taskFile) {
        $taskText = [IO.File]::ReadAllText($taskFile)
        $null = ConvertFrom-Json $taskText
        if ($taskText.Contains('Resolve-Path experimental/snapdragon/tools)')) { throw 'Obsolete inline task root' }
        foreach ($reference in [regex]::Matches($taskText, 'experimental/snapdragon/(?:tools|src)/[A-Za-z0-9_./-]+\.(?:ps1|py|c|h|json)')) {
            if (-not (Test-Path (Join-Path $repo $reference.Value) -PathType Leaf)) { throw ('Missing task source: ' + $reference.Value) }
        }
    }
    $pythonPath = if ([IO.Path]::IsPathRooted($Python)) { $Python } else { Join-Path $repo $Python }
    'import ast,pathlib,sys; files=list(pathlib.Path(sys.argv[1]).rglob("*.py")); [ast.parse(path.read_text(encoding="utf-8"), filename=str(path)) for path in files]; print("PASS Python syntax:",len(files))' | & $pythonPath -B - (Join-Path $root 'tools')
    if ($LASTEXITCODE -ne 0) { throw 'Python syntax check failed' }
    Write-Output ('PASS layout: {0} PowerShell scripts, {1} C/header files, {2} documents, catalogs and task paths' -f $scripts.Count,$sources.Count,$documents.Count)
    return
}

function Write-Field([string]$Name, [object]$Value) {
    if ($null -ne $Value -and "$Value" -ne "") {
        Write-Output ("{0}={1}" -f $Name, "$Value".Trim())
    }
}

Write-Output "[host]"
$computer = Get-CimInstance Win32_ComputerSystem
$product = Get-CimInstance Win32_ComputerSystemProduct
$processor = Get-CimInstance Win32_Processor | Select-Object -First 1
$memory = Get-CimInstance Win32_PhysicalMemory | Select-Object -First 1
$firmware = Get-CimInstance Win32_BIOS
$operatingSystem = Get-CimInstance Win32_OperatingSystem
Write-Field "model" $product.Name
Write-Field "sku" $computer.SystemSKUNumber
Write-Field "architecture" $computer.SystemType
Write-Field "os" $operatingSystem.Caption
Write-Field "os_version" $operatingSystem.Version
Write-Field "os_build" $operatingSystem.BuildNumber
Write-Field "firmware" $firmware.SMBIOSBIOSVersion
Write-Field "firmware_date" $firmware.ReleaseDate.ToString("yyyy-MM-dd")

Write-Output "`n[cpu]"
Write-Field "name" $processor.Name
Write-Field "vendor" $processor.Manufacturer
Write-Field "cores" $processor.NumberOfCores
Write-Field "logical_processors" $processor.NumberOfLogicalProcessors
Write-Field "max_clock_mhz" $processor.MaxClockSpeed
Write-Field "l2_kib" $processor.L2CacheSize

Write-Output "`n[memory]"
Write-Field "capacity_bytes" $memory.Capacity
Write-Field "configured_mts" $memory.ConfiguredClockSpeed
Write-Field "part" $memory.PartNumber

Write-Output "`n[accelerators]"
$accelerators = Get-PnpDevice -PresentOnly | Where-Object {
    $_.Class -eq "ComputeAccelerator" -or $_.Class -eq "Display"
}
foreach ($accelerator in $accelerators) {
    $driver = Get-CimInstance Win32_PnPSignedDriver | Where-Object {
        $_.DeviceID -eq $accelerator.InstanceId
    } | Select-Object -First 1
    Write-Field "device" $accelerator.FriendlyName
    Write-Field "class" $accelerator.Class
    Write-Field "instance" $accelerator.InstanceId
    if ($driver) {
        Write-Field "driver_version" $driver.DriverVersion
        Write-Field "driver_provider" $driver.DriverProviderName
        Write-Field "driver_inf" $driver.InfName
    }
    Write-Output ""
}

Write-Output "[runtime]"
foreach ($name in @("dxcore.dll", "d3d12.dll", "DirectML.dll", "Windows.AI.MachineLearning.dll", "QnnHtp.dll", "QnnSystem.dll")) {
    $path = Join-Path $env:SystemRoot "System32\$name"
    if (Test-Path -LiteralPath $path) {
        $item = Microsoft.PowerShell.Management\Get-Item -LiteralPath $path
        Write-Field $name $item.VersionInfo.FileVersion
    } else {
        Write-Field $name "absent"
    }
}

Write-Output "`n[toolchain]"
foreach ($name in @("clang", "gcc", "dxc", "fxc", "cl", "llvm-dlltool", "llvm-readobj")) {
    $command = Get-Command $name -ErrorAction SilentlyContinue
    Write-Field $name $(if ($command) { $command.Source } else { "absent" })
}

if ($IncludePowerState) {
    Write-Output "`n[power]"
    powercfg.exe /getactivescheme
    powercfg.exe /a
}