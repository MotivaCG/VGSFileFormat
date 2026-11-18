"""PGS Editor MCP adapter (stdio). stdout belongs exclusively to the MCP SDK.

It talks to the live PGS Editor through the editor's private loopback bridge (pgsautomation.h): the
editor writes <app data>/VGS/PGS Editor/mcp/<session>.json with a port and a token while
Edit > Enable MCP is on, and this adapter finds it there.
"""

import argparse
import asyncio
import json
import os
from pathlib import Path
import socket
import sys
import time
from typing import Annotated, Any, Literal

from pydantic import Field

from mcp.server import MCPServer
from mcp.server.mcpserver.exceptions import ToolError
from mcp.types import CallToolResult, ImageContent, TextContent, ToolAnnotations

HERE = Path(__file__).resolve().parent
INSTRUCTIONS = (HERE / "workflows.md").read_text(encoding="utf-8")
mcp = MCPServer("PGS Editor", version="1.0.0", instructions=INSTRUCTIONS)
READ = ToolAnnotations(readOnlyHint=True, openWorldHint=False)
EDIT = ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False)
WRITE = ToolAnnotations(readOnlyHint=False, destructiveHint=True, openWorldHint=False)
MAX_RESPONSE = 64 * 1024 * 1024

_sessions_dir: Path | None = None
_selected: str | None = None


def session_directory() -> Path:
    if _sessions_dir is not None:
        return _sessions_dir
    if sys.platform == "win32":
        return Path(os.environ["APPDATA"]) / "VGS" / "PGS Editor" / "mcp"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "VGS" / "PGS Editor" / "mcp"
    return Path.home() / ".local" / "share" / "VGS" / "PGS Editor" / "mcp"


def _descriptors() -> list[dict[str, Any]]:
    found = []
    for path in sorted(session_directory().glob("*.json")):
        try:
            if path.stat().st_size > 16_384:
                continue
            d = json.loads(path.read_text(encoding="utf-8"))
            if (d.get("format") == "pgs-editor-automation" and d.get("version") == 1 and d.get("host") == "127.0.0.1"
                    and isinstance(d.get("port"), int) and isinstance(d.get("token"), str) and len(d["token"]) >= 32
                    and isinstance(d.get("session_id"), str)):
                found.append(d)
        except (OSError, ValueError, AttributeError):
            continue
    return found


def _exchange(d: dict[str, Any], method: str, arguments: dict[str, Any], timeout: float = 120) -> dict[str, Any]:
    request = json.dumps({"token": d["token"], "method": method, "arguments": arguments},
                         ensure_ascii=False, allow_nan=False).encode("utf-8") + b"\n"
    try:
        with socket.create_connection(("127.0.0.1", d["port"]), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(request)
            with sock.makefile("rb") as stream:
                line = stream.readline(MAX_RESPONSE + 1)
        if not line.endswith(b"\n"):
            raise ToolError("Invalid or oversized editor response.")
        response = json.loads(line)
        if response.get("ok") is not True:
            raise ToolError(response.get("error", "The editor refused the request."))
        result = response["result"]
        if not isinstance(result, dict):
            raise ToolError("Invalid editor result.")
        return result
    except (OSError, ValueError, KeyError) as error:
        raise ToolError(f"Cannot communicate with the PGS Editor: {error}") from error


def _live() -> list[tuple[dict[str, Any], dict[str, Any]]]:
    live = []
    for d in _descriptors():
        try:
            live.append((d, _exchange(d, "state", {}, timeout=1.0)))
        except ToolError:
            continue  # a file left by an editor that closed
    return live


def _descriptor() -> dict[str, Any]:
    global _selected
    if _selected:
        for d in _descriptors():
            if d["session_id"] == _selected:
                return d
        raise ToolError("The selected PGS Editor closed. List and select a session again.")
    live = _live()
    if not live:
        raise ToolError("No PGS Editor with MCP enabled: open it and turn on Edit > Enable MCP.")
    if len(live) != 1:
        raise ToolError("Several PGS Editors are open: call list_sessions and select_session.")
    _selected = live[0][0]["session_id"]
    return live[0][0]


async def _request(method: str, arguments: dict[str, Any] | None = None, timeout: float = 120) -> dict[str, Any]:
    d = await asyncio.to_thread(_descriptor)
    return await asyncio.to_thread(_exchange, d, method, arguments or {}, timeout)


async def _wait(seconds: float) -> dict[str, Any]:
    """Poll the editor until gs2mesh is idle or `seconds` pass; the state, with the end of the log."""
    deadline = time.monotonic() + seconds
    state = await _request("state")
    while state.get("busy") and time.monotonic() < deadline:
        await asyncio.sleep(2)
        state = await _request("state")
    log = await _request("log", {"last": 25})
    state["log_tail"] = log["lines"]
    if state.get("busy"):
        state["note"] = "gs2mesh is still working: call wait_for_gs2mesh again (a capture takes a few minutes, with photos up to about ten; much longer suggests a hang)."
    return state


@mcp.tool(title="List editor sessions", annotations=READ)
async def list_sessions() -> list[dict[str, Any]]:
    """Live PGS Editor windows with MCP enabled: session_id, open capture and project. Does not launch the editor."""
    live = await asyncio.to_thread(_live)
    return [{"session_id": d["session_id"], "ply": s.get("ply"), "project_path": s.get("project_path"),
             "selected": d["session_id"] == _selected} for d, s in live]


@mcp.tool(title="Select editor session", annotations=EDIT)
async def select_session(session_id: str) -> dict[str, Any]:
    """Choose which open PGS Editor the other tools drive, by a session_id from list_sessions."""
    global _selected
    for d, state in await asyncio.to_thread(_live):
        if d["session_id"] == session_id:
            _selected = session_id
            return state
    raise ToolError("No live PGS Editor has that session ID.")


@mcp.tool(title="Editor state", annotations=READ)
async def get_state() -> dict[str, Any]:
    """The open capture (ply, splats, bounds), project path and unsaved changes, normals, photos (COLMAP folder), mesh settings, mesh files and levels (0 = main mesh, then LODs), whether gs2mesh is busy (job, stage, progress 0..1, last_error), and the view (show_mesh, mesh_level, mesh_gray, camera)."""
    return await _request("state")


@mcp.tool(title="Open capture or project", annotations=WRITE)
async def open_file(path: str, discard_unsaved: bool = False) -> dict[str, Any]:
    """Open a 3DGS .ply or a PGS project (.pgsproj) on the editor's computer. Unsaved changes block it unless discard_unsaved=true."""
    return await _request("open", {"path": path, "discard_unsaved": discard_unsaved})


@mcp.tool(title="Save project", annotations=WRITE)
async def save_project(path: str | None = None) -> dict[str, Any]:
    """Save the project (.pgsproj): to `path`, or to its own file when it has one."""
    return await _request("save", {"path": path} if path else {})


@mcp.tool(title="Estimate normals", annotations=EDIT)
async def estimate_normals() -> dict[str, Any]:
    """Capture > Estimate normals: each splat's normal, seen finely (about three seconds for 300 000 splats)."""
    return await _request("estimate_normals", timeout=600)


@mcp.tool(title="Set photos", annotations=EDIT)
async def set_photos(folder: str = "") -> dict[str, Any]:
    """The capture's photos for gs2mesh: a folder with an undistorted COLMAP model and its images (the editor finds the model below it). Empty: no photos (geometry and colour from the splats only)."""
    return await _request("set_photos", {"folder": folder})


@mcp.tool(title="Set mesh settings", annotations=EDIT)
async def set_mesh_settings(
        triangles: Annotated[int | None, Field(ge=1000, le=2000000)] = None,
        lods: Annotated[list[int] | None, Field(max_length=4, description="Triangles of each LOD, e.g. [40000, 3000]")] = None,
        texture_size: Annotated[int | None, Field(ge=256, le=16384)] = None,
        normal_map: bool | None = None,
        voxel_mm: Annotated[float | None, Field(ge=0.5, le=20)] = None,
        virtual_resolution: Annotated[int | None, Field(ge=256, le=4096)] = None,
        denoise: bool | None = None) -> dict[str, Any]:
    """Change the mesh settings kept in the project (only those given). Defaults: 200000 triangles, LODs [40000, 3000], 4096 textures, normal map on, 3 mm voxels."""
    patch = {k: v for k, v in {"triangles": triangles, "lods": lods, "texture_size": texture_size, "normal_map": normal_map,
                               "voxel_mm": voxel_mm, "virtual_resolution": virtual_resolution, "denoise": denoise}.items() if v is not None}
    return await _request("set_mesh_settings", patch)


@mcp.tool(title="Generate mesh", annotations=WRITE)
async def generate_mesh(wait_seconds: Annotated[float, Field(ge=0, le=900)] = 600) -> dict[str, Any]:
    """Run gs2mesh on the capture with the project's settings and photos (no dialogs), replacing the mesh made before. Waits up to wait_seconds for it to finish and returns the state with the end of its log; when it is still working, call wait_for_gs2mesh."""
    await _request("generate_mesh")
    return await _wait(wait_seconds)


@mcp.tool(title="Generate LODs", annotations=WRITE)
async def generate_lods(lods: Annotated[list[int] | None, Field(max_length=4)] = None,
                        wait_seconds: Annotated[float, Field(ge=0, le=900)] = 300) -> dict[str, Any]:
    """Make the LODs of the current main mesh (of its edit when it was sculpted), replacing those made before. lods: triangles of each (default the project's). Waits like generate_mesh."""
    await _request("generate_lods", {"lods": lods} if lods else {})
    return await _wait(wait_seconds)


@mcp.tool(title="Wait for gs2mesh", annotations=READ)
async def wait_for_gs2mesh(wait_seconds: Annotated[float, Field(ge=0, le=900)] = 300) -> dict[str, Any]:
    """Wait until gs2mesh is idle (or wait_seconds pass): the state with the end of its log."""
    return await _wait(wait_seconds)


@mcp.tool(title="Cancel gs2mesh", annotations=EDIT)
async def cancel_gs2mesh() -> dict[str, Any]:
    """Ask gs2mesh to stop at its next checkpoint."""
    return await _request("cancel")


@mcp.tool(title="gs2mesh log", annotations=READ)
async def get_gs2mesh_log(last: Annotated[int, Field(ge=1, le=5000)] = 200) -> dict[str, Any]:
    """The last lines gs2mesh logged in this session (its stages, timings and measurements)."""
    return await _request("log", {"last": last})


@mcp.tool(title="Set view", annotations=EDIT)
async def set_view(show: Literal["splats", "mesh"] | None = None,
                   level: Annotated[int | None, Field(ge=0, le=16, description="0 main mesh, then the LODs")] = None,
                   gray: bool | None = None,
                   target: Annotated[list[float] | None, Field(min_length=3, max_length=3)] = None,
                   yaw: float | None = None,
                   pitch: Annotated[float | None, Field(ge=-90, le=90)] = None,
                   distance: Annotated[float | None, Field(gt=0)] = None,
                   orthographic: bool | None = None) -> dict[str, Any]:
    """What the view shows: the splats or the mesh, which mesh level, the mesh gray (shape only) or textured, and the camera (orbit about target, degrees, metres). Use get_state's bounds to frame the capture."""
    args: dict[str, Any] = {}
    if show is not None:
        args["show"] = show
    if level is not None:
        args["level"] = level
    if gray is not None:
        args["gray"] = gray
    camera = {k: v for k, v in {"target": target, "yaw": yaw, "pitch": pitch, "distance": distance,
                                "orthographic": orthographic}.items() if v is not None}
    if camera:
        args["camera"] = camera
    return await _request("set_view", args)


@mcp.tool(title="View a mesh file", annotations=EDIT)
async def view_mesh_file(path: str, gray: bool | None = None) -> dict[str, Any]:
    """Draw any mesh on disk in the view in place of the project's, for inspection: a gs2mesh dense surface (<name>_dense.ply), another run's .obj, a test's output. gray (default: true unless it has an albedo) shows the shape alone. The project's own mesh comes back with set_view(level=...). Large dense meshes take a few seconds to load."""
    args: dict[str, Any] = {"path": path}
    if gray is not None:
        args["gray"] = gray
    return await _request("view_mesh_file", args, timeout=300)


@mcp.tool(title="Screenshot", annotations=READ)
async def screenshot(width: Annotated[int, Field(ge=256, le=2048)] = 1024) -> CallToolResult:
    """A picture of the editor's view as it is now (set_view first to choose what and from where)."""
    r = await _request("screenshot", {"width": width})
    meta = {"width": r["width"], "height": r["height"]}
    return CallToolResult(content=[TextContent(type="text", text=json.dumps(meta)),
                                   ImageContent(type="image", data=r["png"], mimeType="image/png")], structuredContent=meta)


@mcp.tool(title="Export", annotations=WRITE)
async def export(path: str, include_mesh: bool | None = None,
                 mesh_level: Annotated[int | None, Field(ge=-1, le=16, description="-1: the lowest LOD")] = None,
                 texture_size: Annotated[int | None, Field(ge=0, le=16384)] = None,
                 overwrite: bool = False, uncompressed: bool = False) -> dict[str, Any]:
    """Export the capture: .pgs (splats with normals, a thumbnail of the view and, when include_mesh, a mesh level as glb with its textures at texture_size; uncompressed=true stores its pages without entropy coding, quicker to read and larger) or .ply (splats with normals). An existing file is replaced only with overwrite=true."""
    args: dict[str, Any] = {"path": path, "overwrite": overwrite}
    if uncompressed:
        args["uncompressed"] = True
    if include_mesh is not None:
        args["include_mesh"] = include_mesh
    if mesh_level is not None:
        args["mesh_level"] = mesh_level
    if texture_size is not None:
        args["texture_size"] = texture_size
    return await _request("export", args, timeout=600)


def main() -> None:
    global _sessions_dir
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sessions-dir", type=Path, help="Where the editor writes its session files (default: its app data).")
    args = parser.parse_args()
    _sessions_dir = args.sessions_dir
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
