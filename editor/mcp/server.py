"""VGS Editor MCP adapter. stdout belongs exclusively to the MCP SDK."""

import argparse
import asyncio
import json
import os
from pathlib import Path
import socket
import sys
import threading
import time
from typing import Annotated, Any, Literal
from typing_extensions import TypedDict
from pydantic import ConfigDict, Field, with_config

from mcp.server import MCPServer
from mcp.server.mcpserver.exceptions import ToolError
from mcp.types import CallToolResult, ImageContent, TextContent, ToolAnnotations

HERE = Path(__file__).resolve().parent
WORKFLOW = (HERE / "workflows.md").read_text(encoding="utf-8")
TOOLS_REFERENCE = (HERE / "tools.md").read_text(encoding="utf-8")
mcp = MCPServer("VGS Editor", version="1.3.0", instructions=WORKFLOW)
READ = ToolAnnotations(readOnlyHint=True, openWorldHint=False)
EDIT = ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False)
SET = ToolAnnotations(readOnlyHint=False, destructiveHint=False, idempotentHint=True, openWorldHint=False)
WRITE = ToolAnnotations(readOnlyHint=False, destructiveHint=True, openWorldHint=False)

# These constraints are published by tool discovery and enforced before calling the editor.
NonnegativeInt = Annotated[int, Field(strict=True, ge=0)]
PointSize = Annotated[float, Field(strict=True, ge=1, le=12, allow_inf_nan=False)]
ImageWidth = Annotated[int, Field(strict=True, ge=256, le=2048)]
SampleLimit = Annotated[int, Field(strict=True, ge=1, le=2000)]
BrushRadius = Annotated[float, Field(strict=True, ge=0.001, le=0.25, allow_inf_nan=False, description="Fraction of the shorter viewport image side.")]
Coordinate = Annotated[float, Field(strict=True, ge=0, le=1, allow_inf_nan=False)]
ImagePoint = Annotated[list[Coordinate], Field(min_length=2, max_length=2, description="Normalised [x,y], origin at the full image's top-left.")]
BrushPoints = Annotated[list[ImagePoint], Field(min_length=1, max_length=4096)]
LassoPoints = Annotated[list[ImagePoint], Field(min_length=3, max_length=4096)]
ModifierName = Annotated[str, Field(min_length=1, max_length=120)]
WorldValue = Annotated[float, Field(strict=True, ge=-1e6, le=1e6, allow_inf_nan=False)]
CameraValue = Annotated[float, Field(strict=True, ge=-1e7, le=1e7, allow_inf_nan=False)]
Angle = Annotated[float, Field(strict=True, ge=-36000, le=36000, allow_inf_nan=False)]
ScaleValue = Annotated[float, Field(strict=True, ge=0.0001, le=10000, allow_inf_nan=False)]
WorldVector = Annotated[list[WorldValue], Field(min_length=3, max_length=3)]
CameraVector = Annotated[list[CameraValue], Field(min_length=3, max_length=3)]
RotationVector = Annotated[list[Angle], Field(min_length=3, max_length=3)]
ScaleVector = Annotated[list[ScaleValue], Field(min_length=3, max_length=3)]


@with_config(ConfigDict(extra="forbid", strict=True))
class CameraPatch(TypedDict, total=False):
    target: Annotated[CameraVector, Field(description="Inspection camera target [x,y,z] in world coordinates.")]
    yaw: Annotated[Angle, Field(description="Yaw in degrees.")]
    pitch: Annotated[float, Field(strict=True, ge=-90, le=90, allow_inf_nan=False, description="Pitch in degrees.")]
    roll: Annotated[Angle, Field(description="Roll in degrees.")]
    distance: Annotated[float, Field(strict=True, ge=0.001, le=1e7, allow_inf_nan=False, description="Distance from target in world units.")]
    preset: Annotated[int, Field(strict=True, ge=0, le=6, description="0 perspective, 1 front, 2 back, 3 left, 4 right, 5 top, 6 bottom.")]
    orthographic: Annotated[bool, Field(strict=True, description="Free perspective (preset 0) requires false.")]


@with_config(ConfigDict(extra="forbid", strict=True))
class TransformPatch(TypedDict, total=False):
    position: Annotated[WorldVector, Field(description="Reference translation [x,y,z] in world units.")]
    rotation: Annotated[RotationVector, Field(description="Euler XYZ rotation in degrees.")]
    scale: Annotated[ScaleVector, Field(description="Per-axis scale factors [x,y,z].")]
    shear: Annotated[WorldVector, Field(description="Shear components [XY,XZ,YZ].")]
_selected: str | None = None
_selected_pid: int | None = None
_directory_override: Path | None = None
_http_mode = False
_pinned_session = False
_selection_lock = threading.RLock()
MAX_RESPONSE = 32 * 1024 * 1024


def session_directory() -> Path:
    if _directory_override is not None:
        return _directory_override
    if value := os.environ.get("VGS_EDITOR_MCP_DIR"):
        return Path(value)
    if sys.platform == "win32":
        return Path(os.environ["APPDATA"]) / "VGS" / "VGS Editor" / "mcp"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "VGS" / "VGS Editor" / "mcp"
    return Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "VGS" / "VGS Editor" / "mcp"


def _process_alive(pid: int) -> bool | None:
    """Only return False when the OS confirms the process is gone."""
    if not 0 < pid <= 0xFFFFFFFF:
        return None
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return False if ctypes.get_last_error() == 87 else None
        try:
            code = wintypes.DWORD()
            if not kernel.GetExitCodeProcess(handle, ctypes.byref(code)):
                return None
            return code.value == 259  # STILL_ACTIVE
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except (PermissionError, OSError, OverflowError):
        return None


def _descriptors() -> list[dict[str, Any]]:
    result = []
    for path in sorted(session_directory().glob("*.json")):
        try:
            if path.stat().st_size > 16_384:
                continue
            d = json.loads(path.read_text(encoding="utf-8"))
            if (d.get("format") == "vgs-editor-automation" and d.get("version") == 1
                    and d.get("host") == "127.0.0.1" and isinstance(d.get("port"), int)
                    and 0 < d["port"] < 65536 and isinstance(d.get("token"), str)
                    and len(d["token"]) >= 32 and isinstance(d.get("session_id"), str)
                    and isinstance(d.get("pid"), int) and d["pid"] > 0):
                if _process_alive(d["pid"]) is False:
                    path.unlink(missing_ok=True)
                    continue
                result.append(d)
        except (OSError, ValueError, AttributeError):
            continue
    return result


def _exchange(d: dict[str, Any], method: str, arguments: dict[str, Any], timeout: float = 30) -> dict[str, Any]:
    request = json.dumps({"token": d["token"], "method": method, "arguments": arguments},
                         ensure_ascii=False, allow_nan=False).encode("utf-8") + b"\n"
    if len(request) > 1024 * 1024:
        raise ToolError("Editor request exceeds 1 MiB. Split the operation into smaller steps.")
    try:
        with socket.create_connection(("127.0.0.1", d["port"]), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(request)
            with sock.makefile("rb") as stream:
                line = stream.readline(MAX_RESPONSE + 1)
        if not line.endswith(b"\n") or len(line) > MAX_RESPONSE:
            raise ToolError("Invalid or oversized editor response.")
        response = json.loads(line)
        if not isinstance(response, dict):
            raise ToolError("Invalid editor response.")
        if response.get("ok") is not True:
            raise ToolError(response.get("error", "Editor operation failed."))
        result = response["result"]
        if not isinstance(result, dict):
            raise ToolError("Invalid editor result.")
        if "session_id" in result and result["session_id"] != d["session_id"]:
            raise ToolError("Editor session changed. List and select sessions again.")
        return result
    except (OSError, ValueError, KeyError) as error:
        raise ToolError(f"Cannot communicate with the editor: {error}") from error


def _live_sessions() -> list[tuple[dict[str, Any], dict[str, Any]]]:
    live = []
    for d in _descriptors():
        try:
            live.append((d, _exchange(d, "state", {}, timeout=0.7)))
        except ToolError:
            continue  # abandoned files from crashed sessions are not live editors
    return live


def _descriptor() -> dict[str, Any]:
    with _selection_lock:
        return _descriptor_locked()


def _descriptor_locked() -> dict[str, Any]:
    global _selected, _selected_pid
    if _selected:
        for d in _descriptors():
            if d["session_id"] == _selected:
                _selected_pid = d["pid"]
                return d
        # Recover only after a confirmed process exit, not a timeout or MCP being toggled
        # off in a still-open window. An explicit pin never moves to another process.
        if (_http_mode and not _pinned_session and _selected_pid is not None
                and _process_alive(_selected_pid) is False):
            live = _live_sessions()
            if len(live) == 1:
                _selected = live[0][0]["session_id"]
                _selected_pid = live[0][0]["pid"]
                return live[0][0]
        raise ToolError("The selected editor closed. List and select a session again.")
    live = _live_sessions()
    if not live:
        raise ToolError("No connected VGS Editor. Open the rebuilt editor and enable Edit > Enable MCP.")
    if len(live) != 1:
        raise ToolError("Several editors are open. Call list_editor_sessions and select_editor_session.")
    _selected = live[0][0]["session_id"]
    _selected_pid = live[0][0]["pid"]
    return live[0][0]


async def _request(method: str, arguments: dict[str, Any] | None = None) -> dict[str, Any]:
    d = await asyncio.to_thread(_descriptor)
    return await asyncio.to_thread(_exchange, d, method, arguments or {})


async def _settled(check_error: bool = True, timeout: float = 120) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while True:
        s = await _request("state")
        if check_error and s.get("error"):
            raise ToolError(s["error"])
        if not s.get("loading") and not s.get("processing"):
            return s
        if time.monotonic() >= deadline:
            raise ToolError("Editor is still processing after 120 seconds. Inspect get_editor_state before retrying.")
        await asyncio.sleep(0.1)


def _compact(result: dict[str, Any]) -> dict[str, Any]:
    """Edits answer with a lighter state: get_editor_state and get_project keep the details."""
    if "ready" not in result:
        return result
    compact = {k: v for k, v in result.items() if k != "chunk_boundaries"}
    if isinstance(compact.get("modifiers"), list):
        compact["modifiers"] = [{k: m.get(k) for k in ("id", "name", "type", "enabled")} for m in compact["modifiers"]]
    return compact


async def _edit(method: str, arguments: dict[str, Any] | None = None) -> dict[str, Any]:
    await _settled(check_error=False)
    result = await _request(method, arguments)
    if result.get("error"):
        raise ToolError(result["error"])
    if "ready" in result and (result.get("loading") or result.get("processing")):
        result.update(await _settled())
    return _compact(result)


@mcp.tool(title="List editor sessions", annotations=READ)
async def list_editor_sessions() -> list[dict[str, Any]]:
    """Find live local editor windows with MCP enabled. Returns session_id, source/project paths and selection state, without credentials. Call first; select explicitly if several windows are open. Does not launch the editor. Before cleanup, read vgs-editor://workflow if the server instructions are unavailable."""
    live = await asyncio.to_thread(_live_sessions)
    return [{"session_id": d["session_id"], "pid": d["pid"], "asset": s["asset"],
             "project_path": s["project_path"], "loaded": s["loaded"], "selected": d["session_id"] == _selected}
            for d, s in live]


@mcp.tool(title="Select editor session", annotations=EDIT)
async def select_editor_session(session_id: str) -> dict[str, Any]:
    """Select a listed session_id and return its state. Use to choose among several editors or recover a missing selection. Does not edit the project."""
    return await asyncio.to_thread(_select_session, session_id)


def _select_session(session_id: str) -> dict[str, Any]:
    global _selected, _selected_pid
    with _selection_lock:
        if _pinned_session and _selected is not None and session_id != _selected:
            raise ToolError("This adapter is explicitly pinned to another editor. Run a separate adapter.")
        live = _live_sessions()
        if (_http_mode and _selected is not None and session_id != _selected
                and any(d["session_id"] == _selected for d, _ in live)):
            raise ToolError("This endpoint is pinned to another live editor. Run a separate endpoint to control another window.")
        for d, state in live:
            if d["session_id"] == session_id:
                _selected = session_id
                _selected_pid = d["pid"]
                return state
        raise ToolError("No live editor has that session ID.")


@mcp.tool(title="Inspect editor state", annotations=READ)
async def get_editor_state() -> dict[str, Any]:
    """Read loaded/ready/busy/error, source/project, frame/range/fps, chunk boundaries, modifiers with their settings, camera, display and undo availability. ready requires loaded, paused, decoded data. Edit tools return a compact state (no chunk boundaries, modifiers as id/name/type/enabled); call this or get_project when the details are needed. Use images, not state alone, to choose cleanup regions."""
    return await _request("state")


@mcp.tool(title="Pause playback", annotations=SET)
async def pause_playback() -> dict[str, Any]:
    """Pause at the current playhead before inspection or edits. Returns state; pending decoding may still finish. Does not change geometry or create an undo step."""
    return await _request("pause")


@mcp.tool(title="Import capture", annotations=WRITE)
async def import_capture(path: str, discard_unsaved: bool = False) -> dict[str, Any]:
    """Import a local .vgs/.pgs/.mint capture on the editor PC into a new project (File > Import capture) and wait for decoding. Replaces the current project and resets undo history. Unsaved work blocks it unless discard_unsaved=true explicitly abandons it. Does not overwrite the source. For a .vgsproj use open_project."""
    if Path(path).suffix.lower() not in (".vgs", ".pgs", ".mint"):
        raise ToolError("import_capture takes a .vgs, .pgs or .mint capture; open a .vgsproj with open_project.")
    await _request("open", {"path": path, "discard_unsaved": discard_unsaved})
    return await _settled()


@mcp.tool(title="New project", annotations=WRITE)
async def new_project(discard_unsaved: bool = False) -> dict[str, Any]:
    """File > New project: closes the capture and project, leaving the editor empty with nothing to undo. Unsaved edits block it unless discard_unsaved=true explicitly abandons them."""
    return await _request("new_project", {"discard_unsaved": discard_unsaved})


@mcp.tool(title="Open project", annotations=WRITE)
async def open_project(path: str, discard_unsaved: bool = False) -> dict[str, Any]:
    """Open a saved .vgsproj project on the editor PC (File > Open project) and wait for its capture to decode. Replaces the current project and resets undo history. Unsaved work blocks it unless discard_unsaved=true explicitly abandons it. For a capture use import_capture."""
    if Path(path).suffix.lower() != ".vgsproj":
        raise ToolError("open_project takes a .vgsproj project; import a .vgs, .pgs or .mint with import_capture.")
    await _request("open", {"path": path, "discard_unsaved": discard_unsaved})
    return await _settled()


@mcp.tool(title="Read project settings", annotations=READ)
async def get_project() -> dict[str, Any]:
    """Read the loaded, paused project as native JSON. Use its transform, modifiers and captureSettings fields to construct patches. Read-only; does not save a file."""
    return await _edit("project")


@mcp.tool(title="Seek source frame", annotations=SET)
async def seek_frame(frame: NonnegativeInt) -> dict[str, Any]:
    """Seek a zero-based source frame within inclusive Start/End, or one of an active Loop morph's frames after End (End+1 to End+its frame count), and wait for decoding. Returns state including chunk. No undo step. Refresh stroke images after seeking; picks do not transfer between source chunks."""
    return await _edit("seek", {"frame": frame})


@mcp.tool(title="Set frame range", annotations=EDIT)
async def set_frame_range(start: NonnegativeInt, end: NonnegativeInt) -> dict[str, Any]:
    """Set inclusive zero-based start/end within 0..frames-1. Undoable playback/export-range edit; clamps the playhead. Does not trim the source or restrict modifiers to that interval. Restore a temporarily expanded range."""
    return await _edit("range", {"start": start, "end": end})


@mcp.tool(title="Set points or Gaussian display", annotations=SET)
async def set_render_mode(mode: Literal["points", "splats"], point_size: PointSize | None = None) -> dict[str, Any]:
    """Use points (pointcloud) to inspect detached noise and spatial outliers; splats to validate final appearance and silhouette. point_size is in pixels; omitted/null preserves it. Display change, outside undo history. Refresh stroke images."""
    arguments: dict[str, Any] = {"mode": mode}
    if point_size is not None:
        arguments["point_size"] = point_size
    return await _edit("display", arguments)


@mcp.tool(title="Set inspection view", annotations=SET)
async def set_view(preset: Literal["perspective", "front", "back", "left", "right", "top", "bottom"], focus: bool = True) -> dict[str, Any]:
    """Choose perspective/front/back/left/right/top/bottom for the inspection camera. focus=true reframes visible geometry. Does not move the capture or calibrate rig cameras. Use multiple views and refresh stroke images."""
    return await _edit("view", {"preset": preset, "focus": focus})


@mcp.tool(title="Adjust inspection camera", annotations=SET)
async def set_camera(patch: CameraPatch) -> dict[str, Any]:
    """Adjust the inspection camera using the typed patch fields; omitted fields stay unchanged. Free perspective requires preset=0 and orthographic=false in the resulting camera. Does not move the capture or calibrate rig cameras. Refresh stroke images."""
    return await _edit("camera", patch)


@mcp.tool(title="Capture viewport image", annotations=READ, structured_output=False)
async def get_viewport_image(max_width: ImageWidth = 1024) -> CallToolResult:
    """Return actual paused viewport PNG plus width/height, frame, chunk and view_id. Downscales to max_width without cropping. Requires desktop OpenGL. Orthographic named views (front/back/left/right/top/bottom) also return scale: pixels_per_metre, the world point at the image centre and the world directions of image right/up, for measuring offsets and heights. For strokes, select enabled Erase and enable editing BEFORE this image; refresh after any view/scene change."""
    r = await _edit("snapshot", {"max_width": max_width})
    png = r.pop("png")
    return CallToolResult(content=[TextContent(type="text", text=json.dumps(r)),
                                  ImageContent(type="image", data=png, mimeType="image/png")], structuredContent=r)


@mcp.tool(title="Inspect the capture", annotations=READ, structured_output=False)
async def inspect_capture(
        include_motion: bool = False,
        show_removed: bool = False,
        render: Literal["points", "splats", "current"] = "points",
        motion_frames: Annotated[int, Field(strict=True, ge=2, le=8)] = 6,
        target: CameraVector | None = None,
        distance: Annotated[float, Field(strict=True, ge=0.05, le=1e4, allow_inf_nan=False)] | None = None,
        save_path: str | None = None, overwrite: bool = False) -> CallToolResult:
    """The main way to look at a capture: images rendered by the editor without moving the user's view. Returns `current` (the viewport as shown), `survey` (one 3x3 image of the frame on screen: back-left high, back, back-right low / left, top, right / front-left high, front, front-right low; top view image up is -Z) and, with include_motion, `motion` (top row and front row at motion_frames frames across Start/End, one fixed framing, the first active crop's wire drawn). Framing is automatic on the visible subject without its extreme points; pass target/distance to look closer. render points (default) shows floaters best, splats the final look. show_removed draws what the modifiers remove in red instead of hiding it, from every view: use it to validate crop, Purge Isolated, Erase and Remove green. Metadata give frame, chunk, counts, framing and pixels_per_metre for orthographic cells. Motion decodes frames twice and returns to the frame shown, so it takes seconds: use it for the first survey and the final check. Not for strokes: use get_viewport_image for those. save_path also saves the images as one PNG on the editor PC (File > Export > inspection image); an existing file needs overwrite=true."""
    await _settled()
    arguments: dict[str, Any] = {"include_motion": include_motion, "show_removed": show_removed, "render": render,
                                 "motion_frames": motion_frames}
    if target is not None:
        arguments["target"] = target
    if distance is not None:
        arguments["distance"] = distance
    if save_path is not None:
        arguments["save_path"] = save_path
        arguments["overwrite"] = overwrite
    d = await asyncio.to_thread(_descriptor)
    r = await asyncio.to_thread(_exchange, d, "inspect", arguments, 240 if include_motion else 60)
    content: list[Any] = [TextContent(type="text", text=json.dumps(r["meta"]))]
    for name in ("current", "survey", "motion"):
        if name in r:
            content.append(TextContent(type="text", text=name))
            content.append(ImageContent(type="image", data=r[name], mimeType="image/png"))
    meta = dict(r["meta"])
    if "saved" in r:
        meta["saved"] = r["saved"]
    return CallToolResult(content=content, structuredContent=meta)


@mcp.tool(title="Sample point data", annotations=READ)
async def sample_points(limit: SampleLimit = 200, include_removed: bool = False) -> dict[str, Any]:
    """Read up to limit active decoded points, source IDs, world positions/colours and filter-visible/active counts. include_removed includes filtered points still decoded, not absent records. Counts are independent of camera occlusion. No semantic classification; inspect images."""
    return await _edit("sample_points", {"limit": limit, "include_removed": include_removed})


@mcp.tool(title="Add modifier", annotations=EDIT)
async def add_modifier(type: Literal["crop", "remove-green", "purge-isolated", "erase", "colour", "animate-transform", "walk", "audio", "prune-low-contribution", "bake-antialiasing", "loop-morph"], name: ModifierName | None = None) -> dict[str, Any]:
    """Append/select an enabled undoable modifier across the timeline; returns modifier_id. Crop initially fits the capture. Purge Isolated defaults to neighbour=4, medianPercent=700; raise medianPercent to 900 for a conservative first comparison (higher removes less). Modifier meanings/examples: vgs-editor://tools."""
    args: dict[str, Any] = {"type": type}
    if name is not None:
        args["name"] = name
    return await _edit("add_modifier", args)


@mcp.tool(title="Edit modifier settings", annotations=EDIT)
async def edit_modifier(modifier_id: str, patch: dict[str, Any]) -> dict[str, Any]:
    """Undoably patch an existing modifier_id using native fields from get_project; nested merges preserve omissions, arrays replace. Cannot patch id/type/timeline/erase; use dedicated Erase tools for picks. Refresh images. Modifier behavior and examples: vgs-editor://tools."""
    return await _edit("edit_modifier", {"modifier_id": modifier_id, "patch": patch})


@mcp.tool(title="Select modifier", annotations=SET)
async def select_modifier(modifier_id: str) -> dict[str, Any]:
    """Select an existing modifier_id for its properties and editing tools; does not enable it or create undo history. Before painting: select an enabled Erase, set_erase_editing(true), then inspect a fresh image."""
    return await _edit("select_modifier", {"modifier_id": modifier_id})


@mcp.tool(title="Remove modifier", annotations=EDIT)
async def remove_modifier(modifier_id: str) -> dict[str, Any]:
    """Undoably remove modifier_id and its settings/picks from the full timeline. Source file stays intact. Prefer patch enabled=false for temporary comparison. Returns state; refresh images."""
    return await _edit("remove_modifier", {"modifier_id": modifier_id})


@mcp.tool(title="Reorder modifier", annotations=EDIT)
async def move_modifier(modifier_id: str, position: NonnegativeInt) -> dict[str, Any]:
    """Undoably move modifier_id to a zero-based position, 0..stack_size-1. Order can affect processing, e.g. green filtering after colour correction. Returns state; inspect the result and refresh images."""
    return await _edit("move_modifier", {"modifier_id": modifier_id, "position": position})


@mcp.tool(title="Set display options", annotations=SET)
async def set_display_options(sh_degree: Literal[0, 1, 2, 3, "all"] | None = None, background: Literal["dark", "light"] | None = None,
                              ghost: bool | None = None, ghost_opacity: Annotated[float, Field(strict=True, ge=0, le=1, allow_inf_nan=False)] | None = None,
                              grid: bool | None = None, axes: bool | None = None, front_marker: bool | None = None) -> dict[str, Any]:
    """Preview-only display settings; omitted values are kept. ghost freezes the points shown now as a faint white copy that stays while seeking or transforming, to compare against; ghost_opacity sets how strong it is. grid, axes and front_marker are the View menu's floor grid, coloured axes and origin/front marker (these three are saved with the project). sh_degree limits the spherical-harmonic bands of view-dependent splat colour: 0 shows base colour only, "all" every band the capture has. Compare 0 against "all" from the same camera to tell a view-dependent colour artifact (only some angles) from geometry or opacity. background "light" makes green spill, dark fringes and haze easier to see. Neither changes the project or export; refresh stroke images."""
    arguments: dict[str, Any] = {}
    if sh_degree is not None:
        arguments["sh_degree"] = sh_degree
    if background is not None:
        arguments["background"] = background
    if ghost is not None:
        arguments["ghost"] = ghost
    if ghost_opacity is not None:
        arguments["ghost_opacity"] = ghost_opacity
    for name, value in (("grid", grid), ("axes", axes), ("front_marker", front_marker)):
        if value is not None:
            arguments[name] = value
    return await _edit("display_options", arguments)


@mcp.tool(title="Playback speed, loop and play", annotations=SET)
async def set_playback(speed: Annotated[float, Field(strict=True, ge=0.1, le=4, allow_inf_nan=False)] | None = None, loop: bool | None = None, playing: bool | None = None) -> dict[str, Any]:
    """Playback speed (1 is real time), whether it loops over Start/End (and an active Loop morph's frames), and start or stop it. While playing the editor is busy: other tools wait, so stop it (playing=false or pause_playback) before inspecting or editing."""
    args: dict[str, Any] = {}
    if speed is not None:
        args["speed"] = speed
    if loop is not None:
        args["loop"] = loop
    if playing is not None:
        args["playing"] = playing
    return await _request("playback", args)


@mcp.tool(title="Save viewport image", annotations=WRITE)
async def save_viewport_image(path: str, overwrite: bool = False) -> dict[str, Any]:
    """Save the viewport as it looks now as a PNG on the editor PC (File > Export > Export viewport image). Existing files need overwrite=true."""
    return await _edit("save_viewport_image", {"path": path, "overwrite": overwrite})


@mcp.tool(title="Save settings as preset", annotations=WRITE)
async def save_preset(name: Annotated[str, Field(min_length=1, max_length=120)], overwrite: bool = False) -> dict[str, Any]:
    """Save the current transform, modifiers and export settings as a named editor preset (as Save preset does), for apply_preset later. An existing user preset of that name needs overwrite=true; a built-in one is kept and shadowed."""
    return await _edit("save_preset", {"name": name, "overwrite": overwrite})


@mcp.tool(title="Export the frame as PLY", annotations=WRITE)
async def export_frame_ply(path: str, overwrite: bool = False) -> dict[str, Any]:
    """Start writing the frame shown, edited, as one 3DGS .ply (File > Export > current frame). Returns job_id: poll get_export_status. Not for a Loop morph's frames."""
    return await _edit("export_frame_ply", {"path": path, "overwrite": overwrite})


@mcp.tool(title="Estimate export size", annotations=READ)
async def estimate_export() -> dict[str, Any]:
    """Start estimating the .vgs export's size and bit rate for the current project and range, by exporting sample windows. Returns job_id: poll get_export_status for bytes, average_mbps, peak_mbps and exact. Shares the one export slot."""
    return await _edit("estimate_export", {})


@mcp.tool(title="Add export task", annotations=WRITE)
async def add_export_task(path: str, overwrite: bool = False) -> dict[str, Any]:
    """Save the current project and range as an export task to run later (.vgstask beside the .vgs/.pgs/.mint/.ply output), as File > Export task does. Returns task_path."""
    return await _edit("add_export_task", {"path": path, "overwrite": overwrite})


@mcp.tool(title="List export tasks", annotations=READ)
async def list_export_tasks(folder: str) -> dict[str, Any]:
    """The .vgstask files in a folder of the editor PC: each task's output, frame count, source capture and whether its output already exists."""
    return await _request("list_export_tasks", {"folder": folder})


@mcp.tool(title="Run export task", annotations=WRITE)
async def run_export_task(task_path: str) -> dict[str, Any]:
    """Start running a saved .vgstask in the background. Returns job_id: poll get_export_status. One export at a time across the editor, its task queue and MCP."""
    return await _request("run_export_task", {"task_path": task_path})


@mcp.tool(title="Toggle Crop editing", annotations=SET)
async def set_crop_editing(enabled: bool) -> dict[str, Any]:
    """Show the selected Crop's wire volume in the viewport, with the points it removes marked (red by default) instead of hidden, to check its fit in images. Requires a selected Crop. Any project edit turns it off again. Turn it off before measuring counts or validating the result. Not an undoable edit."""
    return await _edit("crop_editing", {"enabled": enabled})


@mcp.tool(title="Measure points over the timeline", annotations=READ)
async def measure_timeline(samples: Annotated[int, Field(strict=True, ge=1, le=60)] = 12) -> dict[str, Any]:
    """Count and locate the visible subject at `samples` frames spread over Start/End, then return to the current frame. Gives per-frame chunk/counts and totals (roughly how much the current modifiers remove across the whole take) and `extent`: the box the visible points occupy over all sampled frames, in metres, its X/Z centre and the horizontal radius around that centre. Each frame's extremes (0.5% per side of up to 2000 sampled points) are ignored, so a few floaters do not stretch it, but dense noise still does: confirm in images. Use the centre to place a whole group over its full motion and the radius to size a keep cylinder; the floor height must still be judged by eye. Each sample decodes a frame, so larger values take longer. Requires a loaded, paused editor with Crop editing off."""
    state = await _settled()
    if not state.get("ready"):
        raise ToolError("Import a capture or open a project, and pause playback first.")
    if state.get("crop_editing"):
        raise ToolError("Turn Crop editing off first: while editing, removed points are shown and counted.")
    start, end, original = state["start_frame"], state["end_frame"], state["frame"]
    frames = sorted({round(start + (end - start) * (i + 0.5) / samples) for i in range(samples)})
    rows, kept = [], []
    try:
        for frame in frames:
            await _edit("seek", {"frame": frame})
            sample = await _edit("sample_points", {"limit": 2000})
            rows.append({"frame": frame, "chunk": sample["chunk"], "active_points": sample["active_points"],
                         "visible_points": sample["visible_points"]})
            kept.append(_trimmed([p["position"] for p in sample.get("points", [])]))
    finally:
        await _edit("seek", {"frame": original})
    active = sum(r["active_points"] for r in rows)
    visible = sum(r["visible_points"] for r in rows)
    return {"samples": rows, "active_points": active, "visible_points": visible,
            "removed_percent": round(100 * (active - visible) / active, 2) if active else 0.0,
            "extent": _extent([p for frame in kept for p in frame])}


def _trimmed(points: list[list[float]], share: float = 0.005) -> list[list[float]]:
    """The points inside each axis's central range, without the `share` at either end."""
    if len(points) < 20:
        return points
    cut = int(len(points) * share)
    bounds = []
    for axis in range(3):
        values = sorted(p[axis] for p in points)
        bounds.append((values[cut], values[-cut - 1]))
    return [p for p in points if all(lo <= p[a] <= hi for a, (lo, hi) in enumerate(bounds))]


def _extent(points: list[list[float]]) -> dict[str, Any] | None:
    if not points:
        return None
    minimum = [round(min(p[a] for p in points), 3) for a in range(3)]
    maximum = [round(max(p[a] for p in points), 3) for a in range(3)]
    centre = [round((minimum[0] + maximum[0]) / 2, 3), round((minimum[2] + maximum[2]) / 2, 3)]
    radius = max(((p[0] - centre[0]) ** 2 + (p[2] - centre[1]) ** 2) ** 0.5 for p in points)
    return {"minimum": minimum, "maximum": maximum, "centre_xz": centre, "radius_xz": round(radius, 3)}


@mcp.tool(title="Toggle Erase editing", annotations=EDIT)
async def set_erase_editing(enabled: bool) -> dict[str, Any]:
    """Set selected enabled Erase preview: true shows pink picks and allows strokes; false shows the removal result. Does not enable a disabled modifier or add picks. Set BEFORE the stroke image; refresh after toggling. Outside undo history."""
    return await _edit("erase_editing", {"enabled": enabled})


@mcp.tool(title="Erase with brush", annotations=EDIT)
async def erase_brush(view_id: str, points: BrushPoints, radius: BrushRadius = 0.02, mode: Literal["add", "subtract", "replace"] = "add") -> dict[str, Any]:
    """One undoable Erase stroke; requires selected enabled Erase, editing on and fresh image view_id. Uses normalised image points; radius is relative to the shorter side. add marks removal, subtract unmarks, replace replaces current-chunk picks. Selects projected centres THROUGH DEPTH, including hidden subject geometry. Picks persist only within this chunk. Returns stroke hit count, not total removed. Refresh image after each stroke; turn editing off to validate."""
    return await _edit("stroke", {"view_id": view_id, "points": points, "radius": radius, "mode": mode, "tool": "brush"})


@mcp.tool(title="Erase with lasso", annotations=EDIT)
async def erase_lasso(view_id: str, points: LassoPoints, mode: Literal["add", "subtract", "replace"] = "add") -> dict[str, Any]:
    """One undoable closed Erase polygon using normalised image vertices and fresh image view_id. Requires selected enabled Erase and editing on. add marks removal, subtract unmarks, replace replaces current-chunk picks. Selects THROUGH DEPTH. Picks are chunk-local; refresh image after each stroke and turn editing off to validate."""
    return await _edit("stroke", {"view_id": view_id, "points": points, "mode": mode, "tool": "lasso"})


@mcp.tool(title="Clear Erase picks", annotations=EDIT)
async def clear_erased(all_chunks: bool = False) -> dict[str, Any]:
    """Undoably clear selected Erase picks: all_chunks=false restores this chunk; true restores all chunks. Other modifiers may still hide the points. Leaves the modifier in place; preview need not be on. Refresh images."""
    return await _edit("clear_erased", {"all_chunks": all_chunks})


@mcp.tool(title="Read the Loop morph", annotations=READ)
async def get_loop_morph() -> dict[str, Any]:
    """The selected (or active) Loop morph: seconds and frames it adds after End, the frames it occupies, whether it is fitting, and each marker - its Start and End point (placed on the current Start/End frame, placed on another frame and so unused, or missing) and, from four pairs, off_cm: how far its End point is from where the other pairs put it, flagged when far beyond the rest (likely two different points paired). With Start or End shown and three pairs or more, suggestion gives where the next marker probably goes, as a normalised point of the current view (view_id) to pass to place_morph_marker after checking it in a snapshot."""
    return await _edit("loop_morph", {})


@mcp.tool(title="Place a Loop morph marker", annotations=EDIT)
async def place_morph_marker(view_id: str, point: ImagePoint, number: Annotated[int, Field(strict=True, ge=1, le=1000)] | None = None) -> dict[str, Any]:
    """Undoably place a Loop morph marker on the splat under a normalised point of the current view, on the frame shown: seek the Start or End frame and take a snapshot first; view_id must be current. number places or replaces that marker; without it, the next marker that has its point on the other frame but not this one, else a new one. Pair the same point of the subject on both frames, in the same order. Returns placed_marker and side."""
    args: dict[str, Any] = {"view_id": view_id, "point": point}
    if number is not None:
        args["number"] = number
    return await _edit("morph_marker", args)


@mcp.tool(title="Remove Loop morph markers", annotations=EDIT)
async def remove_morph_markers(numbers: list[Annotated[int, Field(strict=True, ge=1, le=1000)]] | None = None, all: bool = False) -> dict[str, Any]:
    """Undoably remove the selected (or active) Loop morph's markers by number, or every one with all=true."""
    args: dict[str, Any] = {"all": all}
    if numbers is not None:
        args["numbers"] = numbers
    return await _edit("remove_morph_markers", args)


@mcp.tool(title="Undo project edit", annotations=EDIT)
async def undo_edit() -> dict[str, Any]:
    """Reverse the last project edit when can_undo=true: modifiers, strokes, range, transform, preset or output settings. View/playhead stay outside history. Cannot undo opening or file writes/exports. Returns state; refresh stroke images."""
    return await _edit("undo")


@mcp.tool(title="Redo project edit", annotations=EDIT)
async def redo_edit() -> dict[str, Any]:
    """Reapply the last undone project edit when can_redo=true. New edits clear redo. Does not repeat file writes or exports. Returns state; refresh images."""
    return await _edit("redo")


@mcp.tool(title="List recent files", annotations=READ)
async def list_recent_files() -> dict[str, Any]:
    """List the editor's File > Open recent entries, most recent first, with whether each still exists. Use a listed path with import_capture (a capture) or open_project (a .vgsproj) when the user refers to a recent or last capture/project. Works without a loaded capture."""
    return await _request("recent_files")


@mcp.tool(title="List capture presets", annotations=READ)
async def list_presets() -> dict[str, Any]:
    """List Capture Tools presets with exact name, local path and system/user origin. Does not apply them. Use a listed name, then inspect the resulting stack after application."""
    return await _request("presets")


@mcp.tool(title="Replace settings with preset", annotations=WRITE)
async def apply_preset(name: str) -> dict[str, Any]:
    """Undoably apply exact listed name. REPLACES reference transform and ENTIRE modifier stack, including Erase picks; applies playback/coordinate-space choices. Preserves source, camera, range and captureSettings. Re-read modifier IDs and validate motion."""
    return await _edit("apply_preset", {"name": name})


@mcp.tool(title="Transform capture", annotations=EDIT)
async def set_capture_transform(patch: TransformPatch) -> dict[str, Any]:
    """Undoably adjust the capture reference transform using typed patch fields; omitted fields stay unchanged. Moves capture geometry, not the inspection camera; independent world-space crops do not follow automatically. Check crop coverage and refresh images."""
    return await _edit("transform", patch)


@mcp.tool(title="Save reversible project", annotations=WRITE)
async def save_project(path: str, overwrite: bool = False) -> dict[str, Any]:
    """Save reversible edits to local .vgsproj path and mark project saved. References, rather than embeds, the source. Updating current project is allowed; a different existing destination needs overwrite=true. Disk write is not undoable. Use start_export for a baked cleaned capture."""
    return await _edit("save", {"path": path, "overwrite": overwrite})


@mcp.tool(title="Set metadata and export options", annotations=EDIT)
async def set_export_settings(patch: dict[str, Any]) -> dict[str, Any]:
    """Undoably patch exact get_project.captureSettings fields, e.g. title/author/tags, shDegree, playbackMode, despill, or normals (bool, off by default) with normalRig ("SMNV1" or "THE4DSCANNER", the studio that recorded it). Unknown/invalid fields fail. Does not change range, select path or launch export. Some settings affect output only; extension selects format, plain is legacy."""
    return await _edit("export_settings", patch)


@mcp.tool(title="Start capture export", annotations=WRITE)
async def start_export(path: str, overwrite: bool = False) -> dict[str, Any]:
    """Start background .vgs/.pgs/.mint export to a local path (or .ply: a numbered 3DGS frame sequence beside it), freezing current project/modifiers/range. One export across GUI, MCP and task queues. Source overwrite is forbidden; existing output needs overwrite=true. Returns job_id, NOT completion: poll status. Does not save the project. Inspect format notes."""
    return await _edit("export", {"path": path, "overwrite": overwrite})


@mcp.tool(title="Read export progress", annotations=READ)
async def get_export_status(job_id: str) -> dict[str, Any]:
    """Poll job_id for running/done/failed/cancelled, progress/path and available error/counts/notes. Only done means success. Latest 32 jobs are retained for this editor process; older IDs expire. Readable while decoding."""
    return await _request("export_status", {"job_id": job_id})


@mcp.tool(title="Cancel export", annotations=EDIT)
async def cancel_export(job_id: str) -> dict[str, Any]:
    """Request job_id cancellation at a progress checkpoint; returned status may still be running. Poll until terminal; nearly finished jobs may succeed. May leave partial output. Does not delete it, discard the project or cancel decoding; not undoable."""
    return await _request("cancel_export", {"job_id": job_id})


@mcp.resource("vgs-editor://workflow")
def workflow() -> str:
    """App purpose, inward-facing camera rig context and subject cleanup (people or objects) with chunk-aware brushes."""
    return WORKFLOW


@mcp.resource("vgs-editor://tools")
def tool_reference() -> str:
    """Modifier meanings and worked cleanup examples. Tool descriptions and schemas come from tool discovery."""
    return TOOLS_REFERENCE


@mcp.prompt()
def clean_noise(detail: str = "Preserve the subject and fine details") -> str:
    """Plan floater/junk cleanup of a volumetric capture (people and/or objects) from an inward-facing camera rig.

    detail states the user's goal and what to preserve. Returns the inspection/cleanup
    workflow plus that objective; requesting this prompt performs no edits. The assistant
    must still inspect frames/images, choose tools and validate motion and fine detail.
    """
    return WORKFLOW + "\nUser's cleanup objective: " + detail


def main() -> None:
    global _directory_override, _selected, _http_mode, _pinned_session
    from http_transport import build_http_app, connection_data_directory, read_token
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sessions-dir", type=Path)
    parser.add_argument("--transport", choices=["stdio", "streamable-http"], default="stdio")
    parser.add_argument("--host", default="127.0.0.1", help="HTTP bind address; default is local only.")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--token-file", type=Path, default=connection_data_directory() / "http-token")
    parser.add_argument("--public-url", help="HTTPS /mcp URL of a separately configured reverse proxy.")
    parser.add_argument("--editor-session", help="Pin this process to a particular editor session ID.")
    args = parser.parse_args()
    _directory_override = args.sessions_dir
    _selected = args.editor_session
    _pinned_session = args.editor_session is not None
    if args.transport == "stdio":
        mcp.run(transport="stdio")
        return
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535.")
    try:
        app = build_http_app(mcp, read_token(args.token_file), args.host, args.public_url)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    _http_mode = True
    import uvicorn
    uvicorn.run(app, host=args.host, port=args.port, access_log=False, log_level="warning")


if __name__ == "__main__":
    main()
