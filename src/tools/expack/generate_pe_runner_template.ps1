param(
    [string]$Compiler = "C:\msys64\ucrt64\bin\clang.exe",
    [string]$TargetTriple = "x86_64-w64-windows-gnu",
    [string]$Source = "src/tools/expack/pe_runner_template.c",
    [string]$OutputInclude = "src/tools/expack/pe_runner_template.inc",
    [string]$BuildDir = "build/expack-runner"
)

$ErrorActionPreference = "Stop"

function Read-U16([byte[]]$Bytes, [int]$Offset) {
    return [int]$Bytes[$Offset] -bor ([int]$Bytes[$Offset + 1] -shl 8)
}

function Read-U32([byte[]]$Bytes, [int]$Offset) {
    return [uint32]([uint32]$Bytes[$Offset] -bor ([uint32]$Bytes[$Offset + 1] -shl 8) -bor ([uint32]$Bytes[$Offset + 2] -shl 16) -bor ([uint32]$Bytes[$Offset + 3] -shl 24))
}

function Write-U32([byte[]]$Bytes, [int]$Offset, [uint32]$Value) {
    $Bytes[$Offset] = [byte]($Value -band 0xff)
    $Bytes[$Offset + 1] = [byte](($Value -shr 8) -band 0xff)
    $Bytes[$Offset + 2] = [byte](($Value -shr 16) -band 0xff)
    $Bytes[$Offset + 3] = [byte](($Value -shr 24) -band 0xff)
}

function Find-Sequence([byte[]]$Bytes, [byte[]]$Needle) {
    for ($i = 0; $i -le $Bytes.Length - $Needle.Length; $i += 1) {
        $ok = $true
        for ($j = 0; $j -lt $Needle.Length; $j += 1) {
            if ($Bytes[$i + $j] -ne $Needle[$j]) { $ok = $false; break }
        }
        if ($ok) { return $i }
    }
    return -1
}

function Compact-Headers([byte[]]$Bytes) {
    if ($Bytes.Length -lt 0x400) { return $Bytes }
    if ($Bytes[0] -ne 0x4d -or $Bytes[1] -ne 0x5a) { throw "not an MZ executable" }
    $peOffset = [int](Read-U32 $Bytes 0x3c)
    if ($Bytes[$peOffset] -ne 0x50 -or $Bytes[$peOffset + 1] -ne 0x45 -or $Bytes[$peOffset + 2] -ne 0 -or $Bytes[$peOffset + 3] -ne 0) { throw "not a PE executable" }
    $sectionCount = Read-U16 $Bytes ($peOffset + 6)
    $optionalSize = Read-U16 $Bytes ($peOffset + 20)
    $optionalOffset = $peOffset + 24
    $sectionOffset = $optionalOffset + $optionalSize
    $sectionTableEnd = $sectionOffset + 40 * $sectionCount
    if ($sectionTableEnd -gt 0x200) { return $Bytes }
    $sizeOfHeadersOffset = $optionalOffset + 60
    $sizeOfHeaders = Read-U32 $Bytes $sizeOfHeadersOffset
    if ($sizeOfHeaders -ne 0x400) { return $Bytes }
    for ($i = 0; $i -lt $sectionCount; $i += 1) {
        $header = $sectionOffset + 40 * $i
        $rawSize = Read-U32 $Bytes ($header + 16)
        $rawPointer = Read-U32 $Bytes ($header + 20)
        if ($rawSize -ne 0 -and $rawPointer -lt 0x400) { return $Bytes }
    }
    Write-U32 $Bytes $sizeOfHeadersOffset 0x200
    for ($i = 0; $i -lt $sectionCount; $i += 1) {
        $header = $sectionOffset + 40 * $i
        $rawSize = Read-U32 $Bytes ($header + 16)
        $rawPointer = Read-U32 $Bytes ($header + 20)
        if ($rawSize -ne 0) { Write-U32 $Bytes ($header + 20) ([uint32]($rawPointer - 0x200)) }
    }
    $compact = New-Object byte[] ($Bytes.Length - 0x200)
    [Array]::Copy($Bytes, 0, $compact, 0, 0x200)
    [Array]::Copy($Bytes, 0x400, $compact, 0x200, $Bytes.Length - 0x400)
    return $compact
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$exe = Join-Path $BuildDir "pe_runner_template.exe"
$args = @(
    "--target=$TargetTriple",
    "-Oz", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-nostdlib", "-fuse-ld=lld", "-Wl,-e,mainCRTStartup", "-Wl,-s", "-Wl,--gc-sections",
    "-lkernel32", $Source, "-o", $exe
)
& $Compiler @args
if ($LASTEXITCODE -ne 0) { throw "runner compile failed" }

$bytes = [IO.File]::ReadAllBytes($exe)
$bytes = Compact-Headers $bytes
$marker = [byte[]](0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11)
$patchOffset = Find-Sequence $bytes $marker
if ($patchOffset -lt 0) { throw "metadata offset marker not found" }

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("static const unsigned char expack_pe_runner_template[] = {")
for ($i = 0; $i -lt $bytes.Length; $i += 12) {
    $end = [Math]::Min($i + 11, $bytes.Length - 1)
    $chunk = for ($j = $i; $j -le $end; $j += 1) { "0x{0:x2}U" -f $bytes[$j] }
    $suffix = if ($i + 12 -lt $bytes.Length) { "," } else { "" }
    $lines.Add("    " + ($chunk -join ", ") + $suffix)
}
$lines.Add("};")
$lines.Add("#define EXPACK_PE_RUNNER_METADATA_OFFSET_PATCH $($patchOffset)U")
Set-Content -LiteralPath $OutputInclude -Value $lines -Encoding ascii
"runner_size=$($bytes.Length) metadata_patch=$patchOffset output=$OutputInclude"
