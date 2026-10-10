# SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
# Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
#
# Proprietary software owned by Víctor M. Feliz.
# ScanMeNow and The4DScanner are licensed for internal use only.
# No ownership, sale, redistribution, sublicensing or modification rights
# are granted. All other rights remain reserved to the copyright holder.
# See LICENSE.md for the limited use grant and applicable terms.
# Other uses require prior written authorisation, subject to mandatory law.

# A folder that runs on a machine without Qt: the editor, the Qt libraries and plugins it
# needs (windeployqt), the MSVC runtime, the built-in presets and ffmpeg.
#
#   powershell -ExecutionPolicy Bypass -File deploy.ps1 -BuildDir <qmake build folder>
#
# The result is <BuildDir>\deploy, emptied first. Qt Creator runs it as a deploy step.
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    # VGSEditor or VGSViewer; by default whichever the build folder holds. The viewer gets no
    # presets and no ffmpeg, and the decoder licence.
    [ValidateSet("", "VGSEditor", "VGSViewer")][string]$App = "",
    [string]$Configuration = "release",
    [string]$QtBin = "C:\Qt\Qt6.8.1\6.8.0\msvc2022_64\bin",
    [string]$Ffmpeg = "D:\Trabajos\THE4DSCANNER\GraciaConverter\Gracia4DGSConverter\build\Desktop_Qt_6_8_0_MSVC2022_64bit-Release\release\tools\ffmpeg.exe"
)
$ErrorActionPreference = "Stop"
$source = $PSScriptRoot
$built = Join-Path $BuildDir $Configuration
if (-not $App) { $App = if (Test-Path (Join-Path $built "VGSViewer.exe")) { "VGSViewer" } else { "VGSEditor" } }
$exe = Join-Path $built "$App.exe"
$out = Join-Path $BuildDir "deploy"
if (-not (Test-Path $exe)) { throw "Build the $Configuration configuration first: $exe is missing." }
$windeployqt = Join-Path $QtBin "windeployqt.exe"
if (-not (Test-Path $windeployqt)) { throw "windeployqt not found in $QtBin." }

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item $exe $out

# windeployqt needs the compiler's environment for --compiler-runtime; Qt Creator provides it,
# and without it the runtime is simply not copied.
$mode = if ($Configuration -eq "debug") { "--debug" } else { "--release" }
& $windeployqt $mode --compiler-runtime --no-translations (Join-Path $out "$App.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)." }

if ($App -eq "VGSViewer") {
    Copy-Item (Join-Path $source "..\decoder\LICENSE.md") $out
    Write-Host "Deployed to $out"
    return
}

# Built-in presets, read-only beside the program.
New-Item -ItemType Directory -Force (Join-Path $out "presets") | Out-Null
Copy-Item (Join-Path $source "presets\*.preset") (Join-Path $out "presets")

# ffmpeg, for the soundtrack of exports: the one the build copied, or the configured one.
$tool = Join-Path $built "tools\ffmpeg.exe"
if (-not (Test-Path $tool)) { $tool = $Ffmpeg }
if (Test-Path $tool) {
    New-Item -ItemType Directory -Force (Join-Path $out "tools") | Out-Null
    Copy-Item $tool (Join-Path $out "tools")
} else {
    Write-Warning "ffmpeg not found: exports from this folder will not mix or cut audio."
}

if (Test-Path (Join-Path $source "LICENSE.md")) { Copy-Item (Join-Path $source "LICENSE.md") $out }
Write-Host "Deployed to $out"
