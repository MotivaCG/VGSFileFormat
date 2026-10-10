# SPDX-License-Identifier: LicenseRef-VGS-Decoder-Proprietary
# Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
# VGS Viewer is supplied under the VGS Decoder licence (../decoder/LICENSE.md).

# Package a qmake or CMake build with its Qt dependencies, MSVC runtime and licence.
# Qt Creator deploy step: powershell.exe -NoProfile -ExecutionPolicy Bypass
# -File "<viewer>\deploy.ps1" -BuildDir "%{buildDir}"
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [ValidateSet("release", "debug", "RelWithDebInfo", "MinSizeRel")][string]$Configuration = "release",
    [string]$QtBin = "C:\Qt\Qt6.8.1\6.8.0\msvc2022_64\bin"
)
$ErrorActionPreference = "Stop"
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$built = Join-Path $BuildDir $Configuration
$exe = Join-Path $built "VGSViewer.exe"
if (-not (Test-Path -LiteralPath $exe)) { $exe = Join-Path $BuildDir "VGSViewer.exe" }
if (-not (Test-Path -LiteralPath $exe)) { throw "Build the $Configuration configuration first: VGSViewer.exe is missing in $BuildDir." }
$windeployqt = Join-Path $QtBin "windeployqt.exe"
if (-not (Test-Path -LiteralPath $windeployqt)) { throw "windeployqt not found in $QtBin." }
$licence = Join-Path $PSScriptRoot "..\decoder\LICENSE.md"
if (-not (Test-Path -LiteralPath $licence)) { throw "VGS Decoder licence not found: $licence" }

$out = [System.IO.Path]::GetFullPath((Join-Path $BuildDir "deploy"))
if (-not $out.StartsWith($BuildDir.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "The deploy directory must be inside the selected build directory."
}
if (Test-Path -LiteralPath $out) {
    if ((Get-Item -LiteralPath $out).Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw "The deploy directory must not be a symbolic link or junction: $out"
    }
    Remove-Item -LiteralPath $out -Recurse -Force
}
New-Item -ItemType Directory -Path $out -Force | Out-Null
Copy-Item -LiteralPath $exe -Destination $out
$mode = if ($Configuration -eq "debug") { "--debug" } else { "--release" }
& $windeployqt $mode --compiler-runtime --no-translations (Join-Path $out "VGSViewer.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)." }
Copy-Item -LiteralPath $licence -Destination $out
Write-Host "Deployed to $out"
