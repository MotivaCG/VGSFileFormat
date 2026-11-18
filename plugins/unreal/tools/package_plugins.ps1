# Precompiled packages of the Unreal plugins, one per engine version: what someone without a
# compiler unzips into <Project>/Plugins (or what Fab takes). For each engine it assembles VGS
# and PGS (CMake targets vgs_unreal_plugin, pgs_unreal_plugin), builds each with RunUAT
# BuildPlugin (Editor, Development and Shipping binaries; the .uplugin gets that EngineVersion),
# drops Intermediate, zips it as <name>-unreal-<version>-UE<engine>-Win64.zip, and checks that
# the zip loads: a blank project with only that plugin is opened by UnrealEditor-Cmd and its
# classes must be there, with nothing rebuilt.
#
#   .\package_plugins.ps1                         every engine under E:\UE_INSTALL (UE_5.6 and later)
#   .\package_plugins.ps1 -Engines 5.8,5.9        some of them
#   .\package_plugins.ps1 -Plugins PGS -SkipCheck
#
# Packages go to <build>\plugins\unreal\Release\packages\UE<engine>\. Building happens in a
# short folder (-WorkDir) because Unreal's intermediate paths pass Windows' 260 characters
# under a long one. Run it from any shell; RunUAT finds Visual Studio itself.

param(
    [string[]]$Engines = @(),
    [string[]]$Plugins = @("VGS", "PGS"),
    [string]$EngineRoot = "E:\UE_INSTALL",
    [string]$BuildDir = "",
    [string]$WorkDir = "D:\Temp\ubp",
    [switch]$SkipCheck
)

$ErrorActionPreference = "Stop"
# "5.8,5.9" arrives as one string through powershell -File: split lists on commas.
$Engines = @($Engines | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$Plugins = @($Plugins | ForEach-Object { $_ -split "," } | Where-Object { $_ })
foreach ($p in $Plugins) { if ($p -notin @("VGS", "PGS")) { throw "Unknown plugin ${p}: VGS or PGS." } }
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo "build_win64" }
$stage = Join-Path $BuildDir "plugins\unreal\Release"
$out = Join-Path $stage "packages"

# Engines: UE_<major>.<minor> folders with RunUAT, 5.6 and later (the plugins' floor).
$available = Get-ChildItem $EngineRoot -Directory -Filter "UE_5.*" |
    Where-Object { Test-Path (Join-Path $_.FullName "Engine\Build\BatchFiles\RunUAT.bat") } |
    ForEach-Object { $_.Name.Substring(3) } |
    Where-Object { [version]$_ -ge [version]"5.6" } | Sort-Object { [version]$_ }
if ($Engines.Count -eq 0) { $Engines = $available }
foreach ($e in $Engines) { if ($available -notcontains $e) { throw "No UE $e with RunUAT under $EngineRoot." } }

foreach ($name in $Plugins) {
    Write-Host "Assembling $name"
    & cmake --build $BuildDir --config Release --target "$($name.ToLower())_unreal_plugin"
    if ($LASTEXITCODE -ne 0) { throw "Assembling $name failed." }
}

# A class each plugin must register once loaded: its runtime actor, and for PGS its importer.
$classes = @{
    VGS = @("/Script/VGSRuntime.VGSCaptureActor")
    PGS = @("/Script/PGSRuntime.PGSSplatActor", "/Script/PGSEditor.PGSSplatFactory")
}

function Test-Package([string]$Engine, [string]$Name, [string]$Package) {
    # A Blueprint-only project holding just the packaged plugin, opened by a commandlet.
    $project = Join-Path $WorkDir "check\UE$Engine-$Name"
    if (Test-Path $project) { Remove-Item -Recurse -Force $project }
    New-Item -ItemType Directory -Force (Join-Path $project "Plugins") | Out-Null
    Copy-Item -Recurse $Package (Join-Path $project "Plugins\$Name")
    @{ FileVersion = 3; EngineAssociation = $Engine; Category = ""; Description = "";
       Plugins = @(@{ Name = $Name; Enabled = $true }, @{ Name = "PythonScriptPlugin"; Enabled = $true }) } |
        ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $project "Check.uproject")
    $script = Join-Path $project "check.py"
    $wanted = ($classes[$Name] | ForEach-Object { "'$_'" }) -join ", "
    @"
import unreal
missing = [c for c in ($wanted,) if unreal.load_class(None, c) is None]
unreal.log('PACKAGECHECK ' + ('missing ' + ', '.join(missing) if missing else 'ok'))
"@ | Set-Content -Encoding utf8 $script
    $editor = Join-Path $EngineRoot "UE_$Engine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
    $log = Join-Path $project "check.log"
    $p = Start-Process -FilePath $editor -PassThru -NoNewWindow -RedirectStandardOutput $log -ArgumentList @(
        "`"$(Join-Path $project 'Check.uproject')`"", "-run=pythonscript", "-script=`"$script`"",
        "-unattended", "-nullrhi", "-nosplash", "-stdout", "-FullStdOutLogOutput")
    if (-not $p.WaitForExit(1800000)) { Stop-Process -Id $p.Id -Force; throw "UE $Engine $Name check timed out." }
    $result = Select-String -Path $log -Pattern "PACKAGECHECK (.*)" | Select-Object -Last 1
    if (-not $result -or $result.Matches[0].Groups[1].Value -ne "ok") {
        throw "UE $Engine ${Name}: the packaged plugin did not load ($(if ($result) { $result.Line } else { 'no result' })); see $log"
    }
    # Nothing may have been compiled: the binaries in the zip are the ones used.
    if (Test-Path (Join-Path $project "Plugins\$Name\Intermediate")) { throw "UE $Engine ${Name}: the plugin was rebuilt on load." }
}

$results = @()
foreach ($engine in $Engines) {
    $uat = Join-Path $EngineRoot "UE_$engine\Engine\Build\BatchFiles\RunUAT.bat"
    foreach ($name in $Plugins) {
        $source = Join-Path $stage "$name\$name.uplugin"
        $version = (Get-Content $source -Raw | ConvertFrom-Json).VersionName
        $package = Join-Path $WorkDir "UE$engine\$name"
        $log = Join-Path $WorkDir "UE$engine-$name.log"
        if (Test-Path $package) { Remove-Item -Recurse -Force $package }
        New-Item -ItemType Directory -Force (Split-Path $package) | Out-Null
        Write-Host "UE $engine ${name}: building (log $log)"
        & cmd /c "`"$uat`" BuildPlugin -Plugin=`"$source`" -Package=`"$package`" -TargetPlatforms=Win64 > `"$log`" 2>&1"
        if ($LASTEXITCODE -ne 0) {
            $results += [pscustomobject]@{ Engine = $engine; Plugin = $name; Result = "build failed: see $log" }
            continue
        }
        Remove-Item -Recurse -Force (Join-Path $package "Intermediate") -ErrorAction SilentlyContinue
        $check = "not checked"
        if (-not $SkipCheck) {
            try { Test-Package $engine $name $package; $check = "loads" }
            catch { $results += [pscustomobject]@{ Engine = $engine; Plugin = $name; Result = "$_" }; continue }
        }
        # The zip holds the plugin folder itself, debug symbols left out (they triple its size).
        $dest = Join-Path $out "UE$engine"
        New-Item -ItemType Directory -Force $dest | Out-Null
        $zip = Join-Path $dest "$($name.ToLower())-unreal-$version-UE$engine-Win64.zip"
        $tmp = Join-Path $WorkDir "zip\$name"
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
        New-Item -ItemType Directory -Force (Split-Path $tmp) | Out-Null
        robocopy $package $tmp /E /XF *.pdb /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "Copying $package failed." }
        if (Test-Path $zip) { Remove-Item $zip }
        # tar (bsdtar, in Windows since 1803): Compress-Archive of PowerShell 5 writes "\" in
        # entry names, which other unzippers (and Fab) take as part of the file name.
        & tar.exe -a -c -f $zip -C (Split-Path $tmp) $name
        if ($LASTEXITCODE -ne 0) { throw "Zipping $name failed." }
        $results += [pscustomobject]@{ Engine = $engine; Plugin = $name; Result = "$check, $([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB: $zip" }
    }
}
$results | Format-Table -AutoSize -Wrap
if ($results | Where-Object { $_.Result -notmatch "^(loads|not checked)" }) { exit 1 }
