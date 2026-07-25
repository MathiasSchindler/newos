param(
    [string]$Compiler = "C:\Program Files\LLVM\bin\clang.exe",
    [string]$Source = "src/compiler/pe_pack_stub_aarch64.c",
    [string]$OutputInclude = "src/compiler/pe_pack_stub_aarch64.inc",
    [string]$BuildDir = "build/normal/.generated/pe-pack-stub-aarch64"
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$object = Join-Path $BuildDir "pe_pack_stub_aarch64.obj"
$arguments = @(
    "--target=aarch64-w64-windows-gnu", "-std=c11", "-Oz", "-ffreestanding", "-fno-builtin",
    "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-ffunction-sections", "-fdata-sections", "-c", $Source, "-o", $object
)

& $Compiler @arguments
if ($LASTEXITCODE -ne 0) { throw "ARM64 PE pack stub compilation failed" }

$bytes = [IO.File]::ReadAllBytes($object)
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add("static const unsigned char pe_pack_stub_aarch64_object[] = {")
for ($offset = 0; $offset -lt $bytes.Length; $offset += 12) {
    $end = [Math]::Min($offset + 11, $bytes.Length - 1)
    $chunk = for ($index = $offset; $index -le $end; $index += 1) { "0x{0:x2}U" -f $bytes[$index] }
    $suffix = if ($offset + 12 -lt $bytes.Length) { "," } else { "" }
    $lines.Add("    " + ($chunk -join ", ") + $suffix)
}
$lines.Add("};")
$lines.Add("#define PE_PACK_STUB_AARCH64_OBJECT_SIZE $($bytes.Length)U")
Set-Content -LiteralPath $OutputInclude -Value $lines -Encoding ascii
"object=$object size=$($bytes.Length)"
"output=$OutputInclude"