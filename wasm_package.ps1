# Builds everything and assembles the decoder into one folder ready to hand out.
#
#   .\wasm_package.ps1
#   .\wasm_package.ps1 -Shared          # DLL instead of a static library
#   .\wasm_package.ps1 -SkipWasm        # no Emscripten on this machine
#
# What comes out is dist\vgsdecoder-<version>\, containing the native library, the
# headers, the sample source, the tools and the WebAssembly module. Nothing from the
# encoder goes in it, and the script refuses to finish if it finds the signing key in
# anything it is about to package.
#
# The encoder is assembled separately into dist\vgsencoder-<version>\, which is ours and
# is not meant to leave the building.

[CmdletBinding()]
param(
    [switch]$Shared,
    [switch]$SkipWasm,
    [string]$EmsdkRoot = $(if ($env:EMSDK_ROOT) { $env:EMSDK_ROOT } else { 'D:\Dependencias\emsdk' })
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$version = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(VGSFileFormat VERSION ([\d.]+)').Matches[0].Groups[1].Value

function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }

# ---- native ---------------------------------------------------------------------------

Step "Native build ($(if ($Shared) { 'shared' } else { 'static' }))"
$nativeDir = "$root\build_win64"
cmake -S $root -B $nativeDir -G "Visual Studio 17 2022" -A x64 "-DVGS_BUILD_SHARED=$(if ($Shared) { 'ON' } else { 'OFF' })" | Out-Null
cmake --build $nativeDir --config Release | Out-Null
cmake --install $nativeDir --config Release | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'the native build failed' }

# ---- WebAssembly -----------------------------------------------------------------------

$wasmDir = "$root\build_wasm"
if (-not $SkipWasm) {
    Step 'WebAssembly build'
    if (-not (Test-Path "$EmsdkRoot\upstream\emscripten")) {
        throw "no Emscripten at $EmsdkRoot; pass -EmsdkRoot or -SkipWasm"
    }
    # The toolchain is set up straight from the unpacked emsdk: emsdk_env.bat needs an
    # activated SDK, which a plain checkout does not have.
    $saved = $env:PATH
    $env:EM_CONFIG = "$EmsdkRoot\.emscripten"
    $node = (Get-ChildItem "$EmsdkRoot\node\*" -Directory | Select-Object -First 1).FullName + '\bin'
    $python = (Get-ChildItem "$EmsdkRoot\python\*" -Directory | Select-Object -First 1).FullName
    $ninja = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
    $env:PATH = "$EmsdkRoot\upstream\emscripten;$node;$python;$ninja;$saved"
    try {
        if (-not (Test-Path "$wasmDir\CMakeCache.txt")) {
            emcmake cmake -S $root -B $wasmDir -G Ninja -DVGS_BUILD_WASM=ON | Out-Null
        }
        cmake --build $wasmDir | Out-Null
        cmake --install $wasmDir | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'the WebAssembly build failed' }
    } finally {
        $env:PATH = $saved
    }
}

# ---- assemble --------------------------------------------------------------------------

Step 'Assembling'
$dist = "$root\dist"
$decoderOut = "$dist\vgsdecoder-$version"
$encoderOut = "$dist\vgsencoder-$version"
Remove-Item -Recurse -Force $decoderOut, $encoderOut -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $decoderOut, $encoderOut | Out-Null

Copy-Item "$nativeDir\INSTALL\decoder\*" $decoderOut -Recurse -Force
Copy-Item "$nativeDir\INSTALL\encoder\*" $encoderOut -Recurse -Force
if (-not $SkipWasm) {
    Copy-Item "$wasmDir\INSTALL\decoder\web" $decoderOut -Recurse -Force
}
# The specification goes with the encoder and not with the decoder. A decoder licence that
# forbids deriving from the library and then encloses the byte layout gives away what it
# just withheld: the specification is precisely what somebody would take the library apart
# to obtain. The encoder package never leaves here, so it carries both.
Copy-Item "$root\FORMAT.md" $encoderOut -Force
New-Item -ItemType Directory -Force "$encoderOut\docs" | Out-Null
Copy-Item "$root\docs\NUMERICAL_CODING.md" "$encoderOut\docs" -Force

# ---- the check that matters ------------------------------------------------------------
#
# The decoder is the half that ships. Nothing in it may contain the signing key, and the
# guarantee should not rest on having remembered which CMake list a file was in.

Step 'Checking the decoder for key material'
$keysHeader = Get-Content "$root\core\crypto\vgskeys.h" -Raw
$seed = [byte[]]([regex]::Matches($keysHeader, '0x([0-9a-fA-F]{2})') | ForEach-Object { [Convert]::ToByte($_.Groups[1].Value, 16) })
if ($seed.Length -lt 32) { throw 'could not read the signing key to check against' }
$seed = $seed[0..31]

$found = @()
foreach ($file in Get-ChildItem $decoderOut -Recurse -File) {
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    for ($i = 0; $i -le $bytes.Length - 32; $i++) {
        if ($bytes[$i] -ne $seed[0]) { continue }
        $match = $true
        for ($j = 1; $j -lt 32; $j++) {
            if ($bytes[$i + $j] -ne $seed[$j]) { $match = $false; break }
        }
        if ($match) { $found += $file.FullName; break }
    }
}
if ($found.Count) {
    Remove-Item -Recurse -Force $decoderOut
    throw "the signing key is in: $($found -join ', ') - the package was deleted"
}

Write-Host "  clean: no key material in $($decoderOut | Split-Path -Leaf)" -ForegroundColor Green

Step 'Done'
Write-Host "  $decoderOut    <- this is the one to hand out"
Write-Host "  $encoderOut    <- ours, contains the signing key"
