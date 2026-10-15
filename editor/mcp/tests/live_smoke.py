"""Run through MCP against a native temporary editor session (invoked by QtTest)."""
import asyncio
import base64
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import time
from contextlib import asynccontextmanager
from urllib.error import HTTPError, URLError
from urllib.request import urlopen

from mcp import Client
from mcp.client.stdio import StdioServerParameters
from mcp.client.streamable_http import streamable_http_client
import httpx2


@asynccontextmanager
async def connected_client(sessions, transport):
    script = str(Path(__file__).parents[1] / "server.py")
    if transport == "stdio":
        params = StdioServerParameters(command=sys.executable, args=[script, "--sessions-dir", sessions])
        async with Client(params, mode="legacy") as client:
            yield client
        return
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    token = secrets.token_urlsafe(48)
    process = subprocess.Popen([sys.executable, script, "--sessions-dir", sessions,
                                "--transport", "streamable-http", "--port", str(port)],
                               env={**os.environ, "VGS_EDITOR_MCP_HTTP_TOKEN": token},
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    url = f"http://127.0.0.1:{port}/mcp"
    try:
        deadline = time.monotonic() + 12
        while True:
            try:
                await asyncio.to_thread(urlopen, url, timeout=.5)
            except HTTPError as error:
                assert error.code == 401
                break
            except (URLError, OSError):
                assert process.poll() is None, process.stderr.read().decode()
                assert time.monotonic() < deadline, "HTTP server did not start"
                await asyncio.sleep(.05)
        async with httpx2.AsyncClient(headers={"Authorization": "Bearer " + token}, trust_env=False) as http:
            async with Client(streamable_http_client(url, http_client=http), mode="legacy") as client:
                yield client
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        process.stdout.close()
        process.stderr.close()


async def main():
    sessions, source, output, images = sys.argv[1:5]
    transport = sys.argv[5] if len(sys.argv) > 5 else "stdio"
    async with connected_client(sessions, transport) as client:
        async def call(name, **arguments):
            r = await client.call_tool(name, arguments)
            if r.is_error:
                raise AssertionError(f"{name}: {r.content}")
            return r.structured_content

        await call("open_capture", path=source)
        s = await call("get_editor_state")
        assert s["loaded"] and s["ready"] and len(s["chunk_boundaries"]) == 3
        await call("set_render_mode", mode="points")
        await call("set_view", preset="front")
        samples = await call("sample_points")
        assert samples["visible_points"] > 0
        purge = await call("add_modifier", type="purge-isolated")
        await call("edit_modifier", modifier_id=purge["modifier_id"], patch={"isolation": {"medianPercent": 900}})
        await call("undo_edit")
        await call("redo_edit")
        await call("edit_modifier", modifier_id=purge["modifier_id"], patch={"enabled": False})
        await call("add_modifier", type="erase")
        await call("set_erase_editing", enabled=True)

        async def image_id(filename):
            if images == "yes":
                r = await client.call_tool("get_viewport_image")
                assert not r.is_error, r.content
                metadata = json.loads(r.content[0].text)
                data = base64.b64decode(r.content[1].data)
                assert data.startswith(b"\x89PNG\r\n\x1a\n")
                Path(output, filename).write_bytes(data)
                return metadata["view_id"]
            return (await call("get_editor_state"))["view_id"]

        view_id = await image_id("points.png")
        painted = await call("erase_brush", view_id=view_id, points=[[.5, .5]], radius=.25)
        assert painted["picked_records"] > 0
        await call("undo_edit")
        await call("seek_frame", frame=2)
        stale = await client.call_tool("erase_brush", {"view_id": view_id, "points": [[.5, .5]]})
        assert stale.is_error
        view_id = await image_id("chunk-2.png")
        selected = await call("erase_lasso", view_id=view_id, points=[[0, 0], [1, 0], [1, 1], [0, 1]])
        assert selected["picked_records"] == 3
        await call("set_erase_editing", enabled=False)
        assert (await call("sample_points"))["visible_points"] == 0
        await call("set_render_mode", mode="splats")
        await image_id("clean-splats.png")
        await call("save_project", path=str(Path(output, "edited.vgsproj")))
        job = await call("start_export", path=str(Path(output, "result.pgs")))
        deadline = time.monotonic() + 30
        while True:
            status = await call("get_export_status", job_id=job["job_id"])
            if status["status"] != "running":
                assert status["status"] == "done", status
                break
            assert time.monotonic() < deadline
            await asyncio.sleep(.05)
        assert Path(output, "result.pgs").stat().st_size > 0
        print(json.dumps({"mcp": "passed", "transport": transport, "images": images, "brush_and_lasso": "passed", "export": status["status"]}))


if __name__ == "__main__":
    asyncio.run(main())
