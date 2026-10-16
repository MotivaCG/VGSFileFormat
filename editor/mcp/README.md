# VGS Editor MCP

Vendor-independent MCP server for the live Qt editor. The same tools, resources, prompts
and images are exposed over **stdio** and **Streamable HTTP**, using the official MCP
Python SDK. The deployed editor includes the Python runtime it needs; a source checkout
needs Python 3.11+. There is no dependency on
Codex, an OpenAI account, a Claude account or a particular model in the server itself.

The editor's private bridge listens only on 127.0.0.1, with a random port and token for
each session. HTTP is an additional standard MCP transport around that local bridge.
The standalone viewer has no bridge and cannot be controlled by it.

## Choose a client

Any MCP client works: the server is the same for all of them, and the client picks the
model. Use the profile that matches how your client writes its configuration.

| Configuration format | Clients | Generated profile |
|---|---|---|
| `mcpServers` with `command`/`args` (stdio) | Claude Code, Claude Desktop, Cursor, Gemini CLI, Windsurf, most others | `stdio.json` |
| `mcpServers` with `url`/`headers` (Streamable HTTP) | Claude Code, Cursor and other HTTP-capable hosts | `http.json` |
| `servers` (VS Code / GitHub Copilot `mcp.json`) | VS Code | `vscode.json` / `vscode-http.json` |
| TOML `[mcp_servers.*]` | Codex | `codex.toml` / `codex-http.toml` |
| SDK objects | OpenAI Agents SDK, Claude Agent SDK, the MCP SDKs, running on the editor's PC | command/args from `stdio.json`, or URL and header from `http.json` |
| Hosted MCP tool (runs in a provider's cloud) | OpenAI Responses API, Claude API MCP connector | a reachable HTTPS endpoint; `openai-responses-tool.json` |

`claude-desktop.json` and `claude-code-http.json` are the earlier names of `stdio.json`
and `http.json`, still written for existing setups.

Compatibility is with MCP-enabled clients, not every model or chat interface. Visual
cleanup also needs a model and host that accept MCP image content. The server remains
the same when changing providers; the client chooses the model and handles its account.
These integrations are based on their published configuration interfaces. Protocol tests
exercise the official SDK with current and legacy handshakes, not each vendor's desktop UI.

## Windows setup

From this folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

A deployed or installed editor ships its own Python runtime with every dependency in
`mcp/python`, so no Python installation or network access is needed: `install.ps1` uses it
and only writes connection profiles under `%LOCALAPPDATA%/VGS/VGS Editor/mcp-client`.
From a source checkout, it creates an isolated Python environment there instead
(Python 3.11+ required). It prints the Python path to use and the profile paths. It does not change any AI client's settings. This also works when the
editor is installed in Program Files: the runtime and credentials belong to the user.
Use `-EnvironmentDir PATH` to choose a different Python environment location.

Open the rebuilt VGSEditor.exe and turn on **Edit > Enable MCP** (off by default; the choice
is remembered). Register the server in your chosen client, then
reload that client. The server can be registered before the editor opens. Each user runs
their own adapter against their own editor, with their own local credentials.

`-Register claude-code,codex` (either or both) also registers the stdio server with those
clients' own CLIs, for the current user; without it the installer changes no client's
settings. `-RegisterCodex` still works and means `-Register codex`. By hand, with the paths
the installer prints:

- Claude Code: `claude mcp add --scope user --transport stdio vgs-editor -- PYTHON_PATH SERVER_PATH`
  (check with `claude mcp list`), or merge `http.json` into its MCP configuration for HTTP.
- Codex: `codex mcp add vgs-editor -- PYTHON_PATH SERVER_PATH`, or merge `codex.toml` into
  its configuration (check with `codex mcp list`).
- Claude Desktop: Developer > Edit Config, then merge the `vgs-editor` entry of `stdio.json`
  into the existing `mcpServers` object, preserving other servers.
- VS Code: merge `vscode.json` into the workspace's `.vscode/mcp.json` or the user `mcp.json`.
- Cursor, Gemini CLI, Windsurf and similar: merge the `vgs-editor` entry of `stdio.json`
  into their `mcpServers` object.

For another MCP host, configure a stdio server with:

```json
{
  "mcpServers": {
    "vgs-editor": {
      "command": "C:/path/to/your/installed/python.exe",
      "args": ["C:/path/to/mcp/server.py"]
    }
  }
}
```

The two paths must refer to the user's installation. Different hosts may wrap these
command/args fields differently; use the generated profile for the selected client.
No model API key is used by this adapter. Normal model usage belongs to the client.

Long tool calls wait at most 120 seconds for the editor (exports return a job at once).
A client whose default tool timeout is shorter needs it raised: the Codex profiles set
`tool_timeout_sec = 180`.

## Streamable HTTP and cloud integrations

Start HTTP explicitly using the Python executable printed by the installer:

```powershell
& 'PYTHON_PATH' 'SERVER_PATH' --transport streamable-http --port 8765
```

The endpoint is `http://127.0.0.1:8765/mcp`. It requires an `Authorization: Bearer TOKEN`
header. The installer generates the token and ready-to-use local HTTP profiles; it does
not print the token. The token and profiles are in the per-user data directory. stdio
does not require this HTTP credential. `--token-file PATH` selects another token file;
`VGS_EDITOR_MCP_HTTP_TOKEN` can override the credential (clients must use the same value).
`VGS_EDITOR_MCP_DATA_DIR` overrides the per-user runtime/profile location.

An HTTP process controls one selected editor window. Use `--editor-session SESSION_ID`
to pin it explicitly; otherwise the first successful selection is pinned. Use separate
processes/ports for different windows or users. stdio clients naturally have independent
processes and selections.

Hosted MCP tools (the OpenAI Responses API's, the Claude API's MCP connector) run in the
provider's cloud and cannot reach a URL on the user's localhost. To use one, configure a reachable HTTPS reverse proxy
to this adapter and start it with `--public-url https://YOUR-DOMAIN/mcp`. This permits that
host/origin while keeping DNS rebinding protection and token authentication enabled.
The default loopback bind remains suitable behind a local reverse proxy. A non-loopback
`--host` requires `--public-url`. No proxy, public hosting or tunnel is created by installation.

For the Responses API, replace the placeholder URL in `openai-responses-tool.json` with
the configured HTTPS URL and add the object to the request's `tools` array. The model and
OpenAI API key are configured by the calling application. Hosts that require OAuth login
rather than configured bearer headers need an OAuth gateway; this adapter provides bearer
authentication and does not pretend to implement a provider's account login.

For locally running OpenAI Agents SDK applications, use `MCPServerStdio` with the generated
command/args, or `MCPServerStreamableHttp` with the URL and authentication header. This route
allows the application to use the local editor without publishing a public endpoint.

## Use

Ask the assistant, for example:

- "Inspect this capture in points and clean the floating noise. Preserve hair and hands,
  check several frames and views, and validate in splats."
- "Use the brush on the remaining noise beside the subject, then check the next chunk."
- "Apply The4DScanner Base and export a copy to D:/Captures/clean.vgs."

The server explains that the app primarily cleans volumetric character captures from a
surrounding camera rig pointing inward. It distinguishes the subject and intentional props
from reconstruction noise and unwanted environment, and calls for preserving fine detail
and the full motion across the requested range. This context does not provide physical
camera calibration or automatic subject segmentation.

The server supplies this purpose and the inspection procedure in its initialization instructions, the
`vgs-editor://workflow` resource and the `clean_noise` prompt. The assistant needs image
understanding to make visual decisions. MCP connects operations; it does not itself
classify bad points or guarantee automatic cleanup quality.

Tools cover session selection, live state/project JSON, playback pause, frame range,
seeking, point/splat display, standard/custom cameras, viewport images, point sampling,
modifiers, presets, reference transforms, brush/lasso, undo/redo, save and async export.
Tool discovery is the source of truth for each tool's description and typed input schema,
including numeric ranges, stroke lengths, optional camera/transform fields and defaults.
Titles and operation hints describe which calls are idempotent or replace existing work.
See [modifier meanings and cleanup examples](tools.md), also exposed as
`vgs-editor://tools`, for the eleven modifier types and an example cleanup sequence.

Version 1.2 uses `erase_brush`, `erase_lasso` and `set_erase_editing` consistently for Erase
operations (previously `paint_brush`, `select_lasso`, `set_eraser_editing`). Reload tool
discovery after updating; update saved workflows using the previous names. Changing render
mode preserves the current point size unless a value is explicitly provided.

Brush and lasso use normalised viewport coordinates. An image includes a `view_id`;
strokes require it. Frame/chunk/camera/size/render/modifier changes invalidate that ID.
Enable Erase editing before taking the image used for a stroke. Picks are chunk-local;
switch Erase editing off to see the result. Each remote stroke is an undo step.

Export returns `job_id` immediately. Poll `get_export_status` until `done`, `failed` or
`cancelled`. It exports a frozen project snapshot, allowing the live editor to continue
working. Cancellation takes effect at the next exporter progress callback and may leave
a partial output. Source files cannot be overwritten by export. Existing output files
require `overwrite=true`. Opening another capture preserves unsaved edits unless
`discard_unsaved=true` is explicitly supplied.

## Session discovery and troubleshooting

On Windows, descriptors are in `%APPDATA%/VGS/VGS Editor/mcp`. They contain the per-session
connection credential and are not uploaded to MCP hosts. The adapter returns metadata,
not tokens. Normal exit removes the descriptor; crashed-session files are ignored when
their connection cannot be reached. With several live windows, select one explicitly.

Set `VGS_EDITOR_MCP_DIR` in both processes to override discovery. Alternatively start the
editor with `--mcp-session-dir DIRECTORY` and adapter with `--sessions-dir DIRECTORY`.
MCP is **off by default**. Enable or disable it with **Edit > Enable MCP**; the choice is
saved for subsequent editor launches. Disabling closes the listener and all active editor
connections and removes the session descriptor. The adapter can reconnect after you enable
it again; take a new snapshot before painting. An export already started continues.
`VGSEditor.exe --no-mcp` starts with the bridge disabled without changing the saved preference;
you can enable it from Edit during that session. Batch exports and smoke-test runs do not
publish editor sessions. Images require a real desktop OpenGL context; offscreen Qt
can test the control logic but cannot produce usable viewport images on Windows.

If a capture is processing, the adapter waits up to 120 seconds. It reports decode/load
errors as tool errors, avoiding modal dialogs for remote operations. Each load/decode has
its own request ID and origin, so unrelated GUI errors keep their normal dialogs and do
not enter the MCP error state. Historical remote errors
remain visible in the state; subsequent valid operations can recover.

After the selected editor process is confirmed to have exited, HTTP automatically recovers
if exactly one editor is live. With several live editors, select explicitly. A timeout or
disabling MCP in a still-open editor does not cause an automatic switch. A live selected
window cannot be replaced through that endpoint; an explicit `--editor-session` pin never
changes. Abandoned descriptors are removed only
when the operating system confirms their process has exited; connection timeouts alone
do not trigger deletion.

## Validation

```powershell
cmake --build ..\build\cmake-msvc --config Release --target VGSEditor automation_tests
ctest --test-dir ..\build\cmake-msvc -C Release -R automation_tests --output-on-failure
& 'PYTHON_PATH' -m unittest discover -s tests -v
```

Native tests use generated captures and temporary settings, covering authentication,
split socket requests, scene validation, undo, chunk boundaries, stale strokes, brush/lasso,
chunk-local picks and async export. Python tests cover stdio and HTTP with current/legacy
handshakes, identical tool discovery, image blocks, HTTP authentication, session pinning,
per-user profile generation and HTTPS requirements for public bindings.
Set `VGS_MCP_TEST_PYTHON` to the adapter's Python executable to include the native-to-MCP
integration test; on a desktop it also captures and checks real viewport images.

Primary references: [official MCP SDK](https://github.com/modelcontextprotocol/python-sdk),
[Codex MCP configuration](https://learn.chatgpt.com/docs/extend/mcp?surface=cli),
[Claude local MCP configuration](https://modelcontextprotocol.io/docs/develop/connect-local-servers),
[Claude Code MCP](https://code.claude.com/docs/en/mcp),
[OpenAI hosted MCP tools](https://developers.openai.com/api/docs/guides/tools-connectors-mcp),
and [OpenAI Agents SDK MCP](https://openai.github.io/openai-agents-python/mcp/).
