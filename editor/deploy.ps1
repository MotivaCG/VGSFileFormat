# SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
# Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
#
# Proprietary software owned by Víctor M. Feliz.
# ScanMeNow and The4DScanner are licensed for internal use only.
# No ownership, sale, redistribution, sublicensing or modification rights
# are granted. All other rights remain reserved to the copyright holder.
# See LICENSE.md for the limited use grant and applicable terms.
# Other uses require prior written authorisation, subject to mandatory law.

# A folder that runs on a machine without Qt or Python: the editor, the Qt libraries and
# plugins it needs (windeployqt), the MSVC runtime, the built-in presets, ffmpeg and the MCP
# adapter with its own embedded Python and dependencies.
#
#   powershell -ExecutionPolicy Bypass -File deploy.ps1 -BuildDir <qmake build folder>
#
# The result is <BuildDir>\deploy, emptied first. Qt Creator runs it as a deploy step.
# -Python is the build machine's Python (3.11+): its version selects the embedded runtime,
# downloaded once from python.org into <BuildDir>\cache unless -PythonEmbedZip is given,
# and its pip installs the adapter's requirements into that runtime.
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [ValidateSet("VGSEditor")][string]$App = "VGSEditor",
    [string]$Configuration = "release",
    [string]$QtBin = "C:\Qt\Qt6.8.1\6.8.0\msvc2022_64\bin",
    [string]$Ffmpeg = "D:\Trabajos\THE4DSCANNER\GraciaConverter\Gracia4DGSConverter\build\Desktop_Qt_6_8_0_MSVC2022_64bit-Release\release\tools\ffmpeg.exe",
    [string]$Python = "python",
    [string]$PythonEmbedZip
)
$ErrorActionPreference = "Stop"
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$source = $PSScriptRoot
$built = Join-Path $BuildDir $Configuration
$exe = Join-Path $built "$App.exe"
$out = [System.IO.Path]::GetFullPath((Join-Path $BuildDir "deploy"))
if (-not (Test-Path $exe)) { throw "Build the $Configuration configuration first: $exe is missing." }
$windeployqt = Join-Path $QtBin "windeployqt.exe"
if (-not (Test-Path $windeployqt)) { throw "windeployqt not found in $QtBin." }

if (-not $out.StartsWith($BuildDir.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "The deploy directory must be inside the selected build directory."
}
if (Test-Path -LiteralPath $out) {
    if ((Get-Item -LiteralPath $out).Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw "The deploy directory must not be a symbolic link or junction: $out"
    }
    Remove-Item -LiteralPath $out -Recurse -Force
}
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item $exe $out

# windeployqt needs the compiler's environment for --compiler-runtime; Qt Creator provides it,
# and without it the runtime is simply not copied.
$mode = if ($Configuration -eq "debug") { "--debug" } else { "--release" }
& $windeployqt $mode --compiler-runtime --no-translations (Join-Path $out "$App.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)." }

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

$mcpOut = Join-Path $out "mcp"
New-Item -ItemType Directory -Force $mcpOut | Out-Null
foreach ($adapterFile in @("server.py", "http_transport.py", "connections.py", "workflows.md", "tools.md", "requirements.txt", "install.ps1", "README.md")) {
    Copy-Item -LiteralPath (Join-Path $source "mcp/$adapterFile") -Destination $mcpOut
}

# The adapter's own Python: the official embeddable runtime with its requirements installed,
# so assistants can start the MCP server without a Python installation or network access.
$pythonVersion = (& $Python -c "import sys; print('%d.%d.%d' % sys.version_info[:3])")
if ($LASTEXITCODE -ne 0 -or -not $pythonVersion) { throw "Python not found ($Python): it is needed to deploy the MCP runtime." }
$pythonVersion = $pythonVersion.Trim(); $parts = $pythonVersion.Split('.')
if ([int]$parts[0] -ne 3 -or [int]$parts[1] -lt 11) { throw "The MCP runtime needs Python 3.11 or newer; $Python is $pythonVersion." }
if (-not $PythonEmbedZip) {
    $cache = Join-Path $BuildDir "cache"
    New-Item -ItemType Directory -Force $cache | Out-Null
    $PythonEmbedZip = Join-Path $cache "python-$pythonVersion-embed-amd64.zip"
    if (-not (Test-Path -LiteralPath $PythonEmbedZip)) {
        $url = "https://www.python.org/ftp/python/$pythonVersion/python-$pythonVersion-embed-amd64.zip"
        Write-Host "Downloading $url"
        Invoke-WebRequest -Uri $url -OutFile "$PythonEmbedZip.part" -UseBasicParsing
        Move-Item -LiteralPath "$PythonEmbedZip.part" -Destination $PythonEmbedZip
    }
}
if (-not (Test-Path -LiteralPath $PythonEmbedZip)) { throw "Embeddable Python not found: $PythonEmbedZip" }
$runtime = Join-Path $mcpOut "python"
Expand-Archive -LiteralPath $PythonEmbedZip -DestinationPath $runtime
$tag = "$($parts[0])$($parts[1])"
$pth = Join-Path $runtime "python$tag._pth"
if (-not (Test-Path -LiteralPath $pth)) { throw "The embeddable Python does not match $pythonVersion (python$tag._pth missing)." }
# Search path: the standard library, the adapter folder above (server.py imports its
# neighbours) and site-packages, with site enabled so the packages' .pth files apply.
Set-Content -LiteralPath $pth -Encoding ascii -Value @("python$tag.zip", ".", "..", "Lib\site-packages", "import site")
$sitePackages = Join-Path $runtime "Lib\site-packages"
& $Python -m pip install --disable-pip-version-check --no-warn-script-location --no-compile `
    --target $sitePackages --platform win_amd64 --python-version "$($parts[0]).$($parts[1])" --implementation cp `
    --only-binary=:all: -r (Join-Path $source "mcp/requirements.txt")
if ($LASTEXITCODE -ne 0) { throw "Could not install the MCP requirements into the embedded Python." }
# pip's command launchers (bin\*.exe) carry this machine's Python path and are never used: the
# server runs as python.exe server.py. RECORD only serves pip's own uninstall.
Remove-Item -LiteralPath (Join-Path $sitePackages "bin") -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath $sitePackages -Directory -Filter "*.dist-info" | ForEach-Object {
    Remove-Item -LiteralPath (Join-Path $_.FullName "RECORD") -Force -ErrorAction SilentlyContinue
}
# The deployed runtime must import the adapter exactly as an assistant will start it.
& (Join-Path $runtime "python.exe") (Join-Path $mcpOut "server.py") --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw "The embedded Python cannot start the MCP server." }
if (Test-Path (Join-Path $source "LICENSE.md")) { Copy-Item (Join-Path $source "LICENSE.md") $out }
Write-Host "Deployed to $out"
