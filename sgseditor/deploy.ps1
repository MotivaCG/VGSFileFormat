# SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
# Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
# SGS Editor is supplied under the VGS Editor licence (../editor/LICENSE.md).

# Package a qmake or CMake build: SGSEditor.exe, its Qt libraries, the MSVC runtime, the
# licence, and gs2mesh with every library it loads (OpenCV, ONNX Runtime with its CUDA and
# TensorRT providers, cuDNN, CUDA) and the ONNX models it uses with photos (FoundationStereo,
# MoGe-2) in a gs2mesh folder beside the exe, about 6.5 GB.
# Qt Creator deploy step: powershell.exe -NoProfile -ExecutionPolicy Bypass
# -File "<sgseditor>\deploy.ps1" -BuildDir "%{buildDir}"
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [ValidateSet("release", "debug", "Release", "RelWithDebInfo", "MinSizeRel")][string]$Configuration = "release",
    [string]$QtBin = "C:\Qt\Qt6.8.1\6.8.0\msvc2022_64\bin",
    # gs2mesh's install (cmake --install of editor/dependencies/gs2mesh_cpp); empty for the submodule's.
    [string]$Gs2MeshBin = "",
    # The ONNX models gs2mesh uses with photos, shipped in gs2mesh/models where the editor looks
    # first: FoundationStereo (stereo between the photos) and MoGe-2 (the faces' relief).
    [string]$StereoModel = "D:\Trabajos\THE4DSCANNER\3dgs2mesh\FoundationStereo\pretrained_models\onnx\foundation_stereo_vits_640x1280.onnx",
    [string]$MogeModel = "D:\Trabajos\ScanMeNow\RaspberrySoft\SMNForge\build\Desktop_Qt_6_8_0_MSVC2022_64bit-Profile\release\onnx\depth\moge-2-vitl-dynamic.onnx"
)
$ErrorActionPreference = "Stop"
# Windows PowerShell has no $PSScriptRoot yet while it reads the parameters.
if (-not $Gs2MeshBin) { $Gs2MeshBin = Join-Path $PSScriptRoot "..\editor\dependencies\gs2mesh_cpp\INSTALL_WIN64\bin" }
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$built = Join-Path $BuildDir $Configuration
$exe = Join-Path $built "SGSEditor.exe"
if (-not (Test-Path -LiteralPath $exe)) { $exe = Join-Path $BuildDir "SGSEditor.exe" }
if (-not (Test-Path -LiteralPath $exe)) { throw "Build the $Configuration configuration first: SGSEditor.exe is missing in $BuildDir." }
$windeployqt = Join-Path $QtBin "windeployqt.exe"
if (-not (Test-Path -LiteralPath $windeployqt)) { throw "windeployqt not found in $QtBin." }
$licence = Join-Path $PSScriptRoot "..\editor\LICENSE.md"
if (-not (Test-Path -LiteralPath $licence)) { throw "VGS Editor licence not found: $licence" }
if (-not (Test-Path -LiteralPath (Join-Path $Gs2MeshBin "gs2mesh.dll"))) {
    throw "gs2mesh is not built: $Gs2MeshBin has no gs2mesh.dll. Build and install editor\dependencies\gs2mesh_cpp (see sgseditor\README.md)."
}

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
& $windeployqt $mode --compiler-runtime --no-translations (Join-Path $out "SGSEditor.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)." }
Copy-Item -LiteralPath $licence -Destination $out
# gs2mesh in a folder of its own: its CUDA and ONNX libraries stay apart from Qt's, and the
# editor finds it there (Gs2Mesh::searchPaths). The command-line tool is not shipped.
$meshOut = Join-Path $out "gs2mesh"
New-Item -ItemType Directory -Path $meshOut -Force | Out-Null
Get-ChildItem -LiteralPath $Gs2MeshBin -File | Where-Object { $_.Extension -eq ".dll" } | Copy-Item -Destination $meshOut
$modelsOut = Join-Path $meshOut "models"
New-Item -ItemType Directory -Path $modelsOut -Force | Out-Null
foreach ($model in @($StereoModel, $MogeModel)) {
    if ($model -and (Test-Path -LiteralPath $model)) { Copy-Item -LiteralPath $model -Destination $modelsOut }
    else { Write-Warning "Model not found, not shipped (meshes with photos will lack it): $model" }
}
$size = (Get-ChildItem -LiteralPath $out -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1GB
Write-Host ("Deployed to {0} ({1:N1} GB)" -f $out, $size)
