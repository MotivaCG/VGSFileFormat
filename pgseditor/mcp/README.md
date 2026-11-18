# PGS Editor MCP

An MCP server (stdio) for the live PGS Editor, built like the VGS Editor's (`editor/mcp`): the
editor opens a private bridge on 127.0.0.1 with a random port and token per session
(`pgsautomation.cpp`), and this adapter speaks MCP to the client and the bridge's protocol to
the editor. The client chooses the model; nothing here depends on one.

## Setup

1. Python 3.11+ with the packages in `requirements.txt`. The VGS Editor's installer
   (`editor/mcp/install.ps1`) already makes such an environment, which this adapter can share:
   `%LOCALAPPDATA%/VGS/VGS Editor/mcp-client/.venv/Scripts/python.exe`.
2. Register the server in the client, e.g. Claude Code:

   ```
   claude mcp add --scope user --transport stdio pgs-editor -- PYTHON_PATH <repo>/pgseditor/mcp/server.py
   ```

3. Open PGSEditor.exe and turn on **Edit > Enable MCP** (off by default; remembered).

The editor writes its session file to `%APPDATA%/VGS/PGS Editor/mcp`; `--sessions-dir` points the
adapter elsewhere.

## Tools

`list_sessions`, `select_session`, `get_state`, `open_file`, `save_project`, `estimate_normals`,
`set_photos`, `set_mesh_settings`, `generate_mesh`, `generate_lods`, `wait_for_gs2mesh`,
`cancel_gs2mesh`, `get_gs2mesh_log`, `set_view`, `screenshot`, `export`. Their descriptions are
the reference (`tools/list`); `workflows.md` is the sequence the server gives as instructions.
