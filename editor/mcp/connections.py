"""Generate local connection profiles without modifying any client's configuration."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import secrets
import sys

from http_transport import connection_data_directory, validate_token


def private_write(path: Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # Create with owner-only permissions on systems supporting POSIX modes.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(data)
    os.chmod(path, 0o600)


def generate(root: Path, python: Path, output: Path, data_dir: Path | None = None) -> list[Path]:
    root, python, output = root.resolve(), python.resolve(), output.resolve()
    data_dir = (data_dir or connection_data_directory()).resolve()
    data_dir.mkdir(parents=True, exist_ok=True)
    token_path = data_dir / "http-token"
    if not token_path.exists():
        try:
            fd = os.open(token_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                stream.write(secrets.token_urlsafe(48) + "\n")
        except FileExistsError:
            pass
    token = validate_token(token_path.read_text(encoding="utf-8"))
    command, server = str(python), str(root / "server.py")
    endpoint = "http://127.0.0.1:8765/mcp"
    # The common "mcpServers" shape (Claude Desktop/Code, Cursor, Gemini CLI, Windsurf and
    # most hosts); VS Code names its object "servers"; Codex uses TOML, written below.
    stdio = {"mcpServers": {"vgs-editor": {"command": command, "args": [server]}}}
    headers = {"Authorization": "Bearer " + token}
    http = {"mcpServers": {"vgs-editor": {"type": "http", "url": endpoint, "headers": headers}}}
    profiles = {
        "stdio.json": stdio,
        "http.json": http,
        "vscode.json": {"servers": {"vgs-editor": {"type": "stdio", "command": command, "args": [server]}}},
        "vscode-http.json": {"servers": {"vgs-editor": {"type": "http", "url": endpoint, "headers": headers}}},
        # Earlier names of the same profiles, kept for existing setups.
        "claude-desktop.json": stdio,
        "claude-code-http.json": http,
        "openai-responses-tool.json": {"type": "mcp", "server_label": "vgs_editor",
                                        "server_url": "https://YOUR-HTTPS-ENDPOINT/mcp", "authorization": token},
    }
    paths = []
    for name, value in profiles.items():
        path = output / name
        private_write(path, json.dumps(value, indent=2, ensure_ascii=False) + "\n")
        paths.append(path)
    codex = f'[mcp_servers.vgs-editor]\ncommand = {json.dumps(command)}\nargs = [{json.dumps(server)}]\ntool_timeout_sec = 180\n'
    codex_http = f'[mcp_servers.vgs-editor]\nurl = {json.dumps(endpoint)}\ntool_timeout_sec = 180\n\n[mcp_servers.vgs-editor.http_headers]\nAuthorization = {json.dumps("Bearer " + token)}\n'
    for name, value in (("codex.toml", codex), ("codex-http.toml", codex_http)):
        path = output / name
        private_write(path, value)
        paths.append(path)
    return paths


def main() -> None:
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=connection_data_directory())
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    for path in generate(root, Path(sys.executable), args.output_dir or args.data_dir / "connections", args.data_dir):
        print(path)  # Never print token-bearing profile contents.


if __name__ == "__main__":
    main()
