# Prepare the adapter's Python and per-user connection profiles. No global packages are changed.
# A deployed editor ships its own runtime in .\python and needs neither Python nor network;
# a source checkout gets an isolated virtual environment instead.
# -Register claude-code,codex also registers the stdio server with those clients' own CLIs;
# any other MCP client uses a generated profile. -RegisterCodex is kept as -Register codex.
param([ValidateSet("claude-code", "codex")][string[]]$Register = @(), [switch]$RegisterCodex,
      [string]$Python = "python", [string]$EnvironmentDir)
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
if ($RegisterCodex) { $Register = @($Register) + "codex" }
foreach ($client in ($Register | Select-Object -Unique)) {
    switch ($client) {
        "claude-code" { & claude mcp add --scope user --transport stdio vgs-editor -- $pythonPath $serverPath }
        "codex" { & codex mcp add vgs-editor -- $pythonPath $serverPath }
    }
    if ($LASTEXITCODE -ne 0) { throw "Could not register the server in $client." }
    Write-Output "Registered vgs-editor in $client."
}
Write-Output "Installed. Connection profiles for any MCP client are in $clientDataPath/connections."
Write-Output "Standard stdio command: $pythonPath"
Write-Output "Arguments: $serverPath"
Write-Output "For HTTP: $pythonPath $serverPath --transport streamable-http"
