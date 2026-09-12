param(
    [switch]$IncludePowerState
)

$ErrorActionPreference = "Stop"

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