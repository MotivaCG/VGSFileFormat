# Prepare the adapter's Python and per-user connection profiles. No global packages are changed.
# A deployed editor ships its own runtime in .\python and needs neither Python nor network;
# a source checkout gets an isolated virtual environment instead.
param([switch]$RegisterCodex, [string]$Python = "python", [string]$EnvironmentDir)
$ErrorActionPreference = "Stop"
$adapterRoot = $PSScriptRoot
$clientDataPath = if ($env:VGS_EDITOR_MCP_DATA_DIR) { $env:VGS_EDITOR_MCP_DATA_DIR } else {
    Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) "VGS/VGS Editor/mcp-client"
}
$bundledPython = Join-Path $adapterRoot "python/python.exe"
if (-not $EnvironmentDir -and (Test-Path -LiteralPath $bundledPython)) {
    $pythonPath = $bundledPython
} else {
    $environmentPath = if ($EnvironmentDir) { $EnvironmentDir } else { Join-Path $clientDataPath ".venv" }
    $pythonPath = Join-Path $environmentPath "Scripts/python.exe"
    if (-not (Test-Path -LiteralPath $pythonPath)) {
        & $Python -m venv $environmentPath
        if ($LASTEXITCODE -ne 0) { throw "Python 3.11 or newer is required." }
    }
    & $pythonPath -m pip install -r (Join-Path $adapterRoot "requirements.txt")
    if ($LASTEXITCODE -ne 0) { throw "Could not install MCP dependencies." }
}
$serverPath = Join-Path $adapterRoot "server.py"
& $pythonPath (Join-Path $adapterRoot "connections.py") --data-dir $clientDataPath
if ($LASTEXITCODE -ne 0) { throw "Could not generate MCP connection profiles." }
if ($RegisterCodex) {
    & codex mcp add vgs-editor -- $pythonPath $serverPath
    if ($LASTEXITCODE -ne 0) { throw "Could not register the server in Codex." }
} else {
    Write-Output "Installed. Choose your MCP client using $clientDataPath/connections."
    Write-Output "Standard stdio command: $pythonPath"
    Write-Output "Arguments: $serverPath"
    Write-Output "For HTTP: $pythonPath $serverPath --transport streamable-http"
}
