"""Protocol tests with the official SDK and a temporary local editor bridge."""
from __future__ import annotations
import asyncio
import importlib.util
import json
import os
import socket
import subprocess
from pathlib import Path
import socketserver
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from mcp import Client
from mcp.client.stdio import StdioServerParameters
from mcp.client.streamable_http import streamable_http_client
import httpx2

SERVER = Path(__file__).resolve().parents[1] / "server.py"
spec = importlib.util.spec_from_file_location("vgs_adapter", SERVER)
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
sys.path.insert(0, str(SERVER.parent))
import connections
import http_transport


class FakeBridge(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self):
        self.state = {"session_id": "test-session", "asset": "test.pgs", "project_path": "", "loaded": True,
                      "loading": False, "processing": False, "ready": True, "error": "", "view_id": "fresh-image",
                      "frame": 0, "chunk": 0, "playing": False}
        self.requests = []
        self.command_errors = {}
        super().__init__(("127.0.0.1", 0), Handler)


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        req = json.loads(self.rfile.readline())
        self.server.requests.append(req)
        if req["token"] != "a" * 64:
            response = {"ok": False, "error": "Unauthorised connection."}
        elif req["method"] in self.server.command_errors:
            response = {"ok": True, "result": {**self.server.state,"error":self.server.command_errors[req["method"]]}}
        elif req["method"] == "snapshot":
            result = {**self.server.state, "width": 1, "height": 1,
                      "png": "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l4sAAAAASUVORK5CYII="}
            response = {"ok": True, "result": result}
        elif req["method"] == "inspect":
            png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l4sAAAAASUVORK5CYII="
            result = {"meta": {"frame": 0, "chunk": 0}, "current": png, "survey": png}
            if req["arguments"].get("include_motion"):
                result["motion"] = png
            response = {"ok": True, "result": result}
        elif req["method"] == "seek":
            self.server.state["frame"] = req["arguments"]["frame"]
            response = {"ok": True, "result": self.server.state}
        elif req["method"] == "sample_points":
            frame = self.server.state["frame"]
            # A 2 m x 1.9 m wall of points at z=0.5, plus one floater far away.
            points = [{"position": [(i % 20) * 0.1 - 1, (i // 20) * 0.1, 0.5]} for i in range(400)]
            points.append({"position": [9, 9, 9]})
            response = {"ok": True, "result": {"chunk": frame // 10, "frame": frame, "active_points": 100,
                                               "visible_points": 90 if frame < 10 else 70, "points": points}}
        elif req["method"] == "stroke":
            if req["arguments"]["view_id"] != self.server.state["view_id"]:
                response = {"ok": False, "error": "Stale view: take a new snapshot."}
            else:
                response = {"ok": True, "result": {**self.server.state, "picked_records": 3}}
        else:
            response = {"ok": True, "result": self.server.state}
        self.wfile.write(json.dumps(response).encode() + b"\n")


class ServerTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.bridge = FakeBridge()
        self.thread = threading.Thread(target=self.bridge.serve_forever, daemon=True)
        self.thread.start()
        self.descriptor = {"format": "vgs-editor-automation", "version": 1, "session_id": "test-session",
                           "pid": os.getpid(), "host": "127.0.0.1", "port": self.bridge.server_address[1], "token": "a" * 64}
        (self.root / "test.json").write_text(json.dumps(self.descriptor))
        self.params = StdioServerParameters(command=sys.executable, args=[str(SERVER), "--sessions-dir", str(self.root)])

    def tearDown(self):
        self.bridge.shutdown()
        self.bridge.server_close()
        self.thread.join()
        self.temp.cleanup()
        adapter._selected = None
        adapter._selected_pid = None
        adapter._directory_override = None
        adapter._http_mode = False
        adapter._pinned_session = False

    async def test_render_mode_omission_preserves_the_existing_point_size(self):
        async with Client(self.params) as client:
            for arguments in ({"mode":"splats"}, {"mode":"points","point_size":None}, {"mode":"points","point_size":5}):
                result=await client.call_tool("set_render_mode",arguments)
                self.assertFalse(result.is_error)
                sent=[r["arguments"] for r in self.bridge.requests if r["method"]=="display"][-1]
                self.assertEqual("point_size" in sent, arguments.get("point_size") is not None)
                if arguments.get("point_size") is not None:
                    self.assertEqual(sent["point_size"],5)

    async def test_edits_return_compact_state_and_state_keeps_details(self):
        self.bridge.state.update({"chunk_boundaries": [{"seconds": 1, "frame": 30}],
                                  "modifiers": [{"id": "m", "name": "Crop", "type": "crop", "enabled": True, "crop": {"radius": 1}}]})
        async with Client(self.params) as client:
            edited = (await client.call_tool("set_view", {"preset": "front"})).structured_content
            self.assertNotIn("chunk_boundaries", edited)
            self.assertEqual(edited["modifiers"], [{"id": "m", "name": "Crop", "type": "crop", "enabled": True}])
            full = (await client.call_tool("get_editor_state", {})).structured_content
            self.assertIn("chunk_boundaries", full)
            self.assertEqual(full["modifiers"][0]["crop"], {"radius": 1})
            tools = {tool.name: tool for tool in (await client.list_tools()).tools}
            self.assertEqual(tools["get_viewport_image"].input_schema["properties"]["max_width"]["default"], 1024)

    async def test_inspection_returns_labelled_images_and_forwards_options(self):
        async with Client(self.params) as client:
            result = await client.call_tool("inspect_capture", {})
            self.assertFalse(result.is_error)
            images = [item for item in result.content if item.type == "image"]
            names = [item.text for item in result.content if item.type == "text"][1:]
            self.assertEqual((len(images), names), (2, ["current", "survey"]))
            self.assertEqual(result.structured_content["frame"], 0)
            sent = [r["arguments"] for r in self.bridge.requests if r["method"] == "inspect"][-1]
            self.assertEqual(sent, {"include_motion": False, "show_removed": False, "render": "points", "motion_frames": 6})
            result = await client.call_tool("inspect_capture", {"include_motion": True, "show_removed": True,
                                                                "target": [0, 1, 0], "distance": 2})
            self.assertEqual(len([item for item in result.content if item.type == "image"]), 3)
            sent = [r["arguments"] for r in self.bridge.requests if r["method"] == "inspect"][-1]
            self.assertEqual((sent["target"], sent["distance"], sent["show_removed"]), ([0, 1, 0], 2, True))
            self.assertTrue((await client.call_tool("inspect_capture", {"motion_frames": 9})).is_error)

    async def test_display_options_send_only_what_is_given(self):
        async with Client(self.params) as client:
            for arguments, sent in (({}, {}), ({"sh_degree": 0}, {"sh_degree": 0}),
                                    ({"sh_degree": "all", "background": "light"}, {"sh_degree": "all", "background": "light"})):
                result = await client.call_tool("set_display_options", arguments)
                self.assertFalse(result.is_error)
                self.assertEqual([r["arguments"] for r in self.bridge.requests if r["method"] == "display_options"][-1], sent)
            self.assertTrue((await client.call_tool("set_display_options", {"sh_degree": 4})).is_error)

    async def test_timeline_measure_spreads_samples_and_restores_the_frame(self):
        self.bridge.state.update({"start_frame": 0, "end_frame": 19, "frame": 7})
        async with Client(self.params) as client:
            result = await client.call_tool("measure_timeline", {"samples": 4})
            self.assertFalse(result.is_error)
            data = result.structured_content
            self.assertEqual([s["frame"] for s in data["samples"]], [2, 7, 12, 17])
            self.assertEqual((data["active_points"], data["visible_points"], data["removed_percent"]), (400, 320, 20.0))
            extent = data["extent"]
            self.assertEqual((extent["minimum"], extent["maximum"]), ([-1, 0, 0.5], [0.9, 1.9, 0.5]))
            self.assertEqual((extent["centre_xz"], extent["radius_xz"]), ([-0.05, 0.5], 0.95))
            seeks = [r["arguments"]["frame"] for r in self.bridge.requests if r["method"] == "seek"]
            self.assertEqual(seeks[-1], 7)
            self.bridge.state["crop_editing"] = True
            self.assertTrue((await client.call_tool("measure_timeline", {})).is_error)

    async def test_discovered_schemas_titles_and_operation_hints(self):
        async with Client(self.params) as client:
            tools={tool.name:tool for tool in (await client.list_tools()).tools}
            self.assertTrue({"erase_brush","erase_lasso","set_erase_editing"} <= tools.keys())
            self.assertFalse({"paint_brush","select_lasso","set_eraser_editing"} & tools.keys())
            self.assertTrue(all(tool.title for tool in tools.values()))
            for name in ("seek_frame","set_view","set_camera","set_render_mode","select_modifier","pause_playback"):
                self.assertTrue(tools[name].annotations.idempotent_hint,name)
            self.assertTrue(tools["apply_preset"].annotations.destructive_hint)
            for name,key,minimum,maximum in (("get_viewport_image","max_width",256,2048),("sample_points","limit",1,2000),("erase_brush","radius",.001,.25)):
                field=tools[name].input_schema["properties"][key]
                self.assertEqual((field["minimum"],field["maximum"]),(minimum,maximum))
            point_size=tools["set_render_mode"].input_schema["properties"]["point_size"]
            self.assertIsNone(point_size["default"])
            numeric=next(item for item in point_size["anyOf"] if item["type"]=="number")
            self.assertEqual((numeric["minimum"],numeric["maximum"]),(1,12))
            for name,count in (("erase_brush",1),("erase_lasso",3)):
                points=tools[name].input_schema["properties"]["points"]
                self.assertEqual((points["minItems"],points["maxItems"]),(count,4096))
                self.assertEqual((points["items"]["minItems"],points["items"]["maxItems"]),(2,2))
                self.assertEqual(points["items"]["items"]["maximum"],1)
            for name,model,fields in (("set_camera","CameraPatch",{"target","yaw","pitch","roll","distance","preset","orthographic"}),
                                       ("set_capture_transform","TransformPatch",{"position","rotation","scale","shear"})):
                schema=tools[name].input_schema["$defs"][model]
                self.assertEqual(set(schema["properties"]),fields)
                self.assertFalse(schema["additionalProperties"])
                self.assertFalse(schema.get("required",[]))

    async def test_invalid_schema_inputs_never_reach_the_editor(self):
        invalid=[
            ("get_viewport_image",{"max_width":255}), ("get_viewport_image",{"max_width":2049}),
            ("sample_points",{"limit":0}), ("sample_points",{"limit":2001}),
            ("set_render_mode",{"mode":"points","point_size":0}), ("set_render_mode",{"mode":"splats","point_size":13}),
            ("set_render_mode",{"mode":"points","point_size":True}),
            ("seek_frame",{"frame":-1}), ("seek_frame",{"frame":True}),
            ("erase_brush",{"view_id":"fresh-image","points":[],"radius":.02}),
            ("erase_brush",{"view_id":"fresh-image","points":[[.5,.5]],"radius":.0009}),
            ("erase_brush",{"view_id":"fresh-image","points":[[.5,.5]],"radius":.251}),
            ("erase_brush",{"view_id":"fresh-image","points":[[1.01,.5]]}),
            ("erase_brush",{"view_id":"fresh-image","points":[[.5]]}),
            ("erase_brush",{"view_id":"fresh-image","points":[[.5,.5,0]]}),
            ("erase_brush",{"view_id":"fresh-image","points":[[.5,.5]]*4097}),
            ("erase_lasso",{"view_id":"fresh-image","points":[[0,0],[1,1]]}),
            ("erase_lasso",{"view_id":"fresh-image","points":[[.5,.5]]*4097}),
            ("set_camera",{"patch":{"distance":0}}), ("set_camera",{"patch":{"pitch":91}}),
            ("set_camera",{"patch":{"target":[0,0]}}), ("set_camera",{"patch":{"unknown":5}}),
            ("set_camera",{"patch":{"target":None}}), ("set_capture_transform",{"patch":{"scale":[0,1,1]}}),
            ("set_camera",{"patch":{"yaw":True}}),
            ("set_capture_transform",{"patch":{"rotation":[0,0,40000]}}),
            ("set_capture_transform",{"patch":{"position":[0,0,0,0]}}),
            ("set_capture_transform",{"patch":{"unknown":5}}),
        ]
        async with Client(self.params) as client:
            for name,arguments in invalid:
                with self.subTest(tool=name,arguments=list(arguments)):
                    result=await client.call_tool(name,arguments)
                    self.assertTrue(result.is_error,name)
            self.assertEqual(self.bridge.requests,[])
            # Valid partial patches forward only the supplied keys; no default/null fields.
            for name,method,arguments in (("set_camera","camera",{"yaw":45}),
                                          ("set_capture_transform","transform",{"position":[0,1,0]})):
                result=await client.call_tool(name,{"patch":arguments})
                self.assertFalse(result.is_error)
                sent=[request for request in self.bridge.requests if request["method"]==method][-1]
                self.assertEqual(sent["arguments"],arguments)

    async def test_explicit_session_pin_survives_a_missing_editor(self):
        adapter._directory_override=self.root
        adapter._selected="explicitly-pinned-session"
        adapter._pinned_session=True
        with self.assertRaisesRegex(adapter.ToolError,"explicitly pinned"):
            await adapter.select_editor_session("test-session")
        self.assertEqual(adapter._selected,"explicitly-pinned-session")

    async def test_confirmed_dead_process_descriptors_are_removed(self):
        process=subprocess.Popen([sys.executable,"-c","pass"])
        process.wait(timeout=5)
        self.assertIs(adapter._process_alive(process.pid),False)
        abandoned=self.root / "abandoned.json"
        abandoned.write_text(json.dumps({**self.descriptor,"pid":process.pid,"session_id":"dead"}))
        adapter._directory_override=self.root
        self.assertEqual(len(adapter._descriptors()),1)
        self.assertFalse(abandoned.exists())
        with patch.object(adapter,"_process_alive",return_value=None):
            self.assertEqual(len(adapter._descriptors()),1)
            self.assertTrue((self.root / "test.json").exists())

    async def test_real_stdio_discovery_tools_images_and_errors(self):
        async with Client(self.params) as client:
            tools = await client.list_tools()
            names = {t.name for t in tools.tools}
            self.assertTrue({"get_viewport_image", "erase_brush", "erase_lasso", "undo_edit", "start_export"} <= names)
            self.assertIn("chunk boundaries", client.instructions)
            sessions = await client.call_tool("list_editor_sessions")
            self.assertFalse(sessions.is_error)
            result = await client.call_tool("select_editor_session", {"session_id": "test-session"})
            self.assertFalse(result.is_error)
            image = await client.call_tool("get_viewport_image")
            self.assertFalse(image.is_error)
            self.assertEqual([c.type for c in image.content], ["text", "image"])
            metadata = json.loads(image.content[0].text)
            self.assertNotIn("png", metadata)
            self.assertEqual(metadata["view_id"], "fresh-image")
            stroke = await client.call_tool("erase_brush", {"view_id": metadata["view_id"], "points": [[.5, .5]]})
            self.assertFalse(stroke.is_error)
            sent = [r for r in self.bridge.requests if r["method"] == "stroke"][-1]
            self.assertEqual(sent["arguments"]["tool"], "brush")
            self.assertEqual(sent["arguments"]["radius"], .02)
            stale = await client.call_tool("erase_brush", {"view_id": "old-image", "points": [[.5, .5]]})
            self.assertTrue(stale.is_error)
            self.assertIn("Stale view", stale.content[0].text)

    async def test_legacy_initialize_handshake(self):
        async with Client(self.params, mode="legacy") as client:
            result = await client.call_tool("get_editor_state")
            self.assertFalse(result.is_error)
            self.assertEqual(result.structured_content["chunk"], 0)

    async def test_multiple_sessions_require_selection_and_tokens_are_private(self):
        adapter._directory_override = self.root
        extra = {**self.descriptor, "session_id": "other-session"}
        # The server returns its actual session ID, so a stale/reused port is ignored.
        (self.root / "stale.json").write_text(json.dumps(extra))
        live = adapter._live_sessions()
        self.assertEqual(len(live), 1)
        result = await adapter.list_editor_sessions()
        self.assertNotIn("token", result[0])
        self.assertNotIn("port", result[0])
        self.assertNotIn("a" * 64, json.dumps(result))
        other = FakeBridge()
        other.state["session_id"] = "other-session"
        worker = threading.Thread(target=other.serve_forever, daemon=True)
        worker.start()
        try:
            (self.root / "other.json").write_text(json.dumps({**extra, "port": other.server_address[1]}))
            with self.assertRaises(adapter.ToolError):
                adapter._descriptor()
            selected = await adapter.select_editor_session("test-session")
            self.assertEqual(selected["session_id"], "test-session")
            self.assertEqual(adapter._descriptor()["session_id"], "test-session")
        finally:
            other.shutdown()
            other.server_close()
            worker.join()

    async def test_invalid_or_nonlocal_descriptors_are_ignored(self):
        adapter._directory_override = self.root
        (self.root / "broken.json").write_text("invalid JSON")
        (self.root / "remote.json").write_text(json.dumps({**self.descriptor, "host": "example.com"}))
        self.assertEqual(len(adapter._descriptors()), 1)

    async def test_decoder_error_is_reported_and_a_timeout_is_not_success(self):
        adapter._directory_override = self.root
        self.bridge.state.update(processing=True, ready=False, error="Decode failed")
        with self.assertRaises(adapter.ToolError):
            await adapter._settled()
        self.bridge.state["error"] = ""
        with self.assertRaises(adapter.ToolError):
            await adapter._settled(timeout=0.01)

    async def test_synchronous_command_errors_are_not_reported_as_success(self):
        self.bridge.command_errors["camera"]="Camera operation failed synchronously"
        async with Client(self.params) as client:
            result=await client.call_tool("set_camera",{"patch":{"yaw":20}})
            self.assertTrue(result.is_error)
            self.assertIn("failed synchronously",result.content[0].text)
            state=await client.call_tool("get_editor_state")
            self.assertFalse(state.is_error)  # State remains inspectable after a tool failure.

    async def test_http_recovers_only_after_exit_and_with_one_live_editor(self):
        process=subprocess.Popen([sys.executable,"-c","pass"]);process.wait(timeout=5)
        adapter._directory_override=self.root;adapter._http_mode=True
        adapter._selected="exited-session";adapter._selected_pid=process.pid
        self.assertEqual(adapter._descriptor()["session_id"],"test-session")
        adapter._selected="disabled-session";adapter._selected_pid=os.getpid()
        with self.assertRaises(adapter.ToolError): adapter._descriptor()  # Toggle off != process exit.
        adapter._selected="exited-session";adapter._selected_pid=process.pid;adapter._pinned_session=True
        with self.assertRaises(adapter.ToolError): adapter._descriptor()
        adapter._pinned_session=False
        other=FakeBridge();other.state["session_id"]="other-session"
        worker=threading.Thread(target=other.serve_forever,daemon=True);worker.start()
        try:
            (self.root / "other.json").write_text(json.dumps({**self.descriptor,"session_id":"other-session","port":other.server_address[1]}))
            with self.assertRaises(adapter.ToolError): adapter._descriptor()
            self.assertEqual(adapter._selected,"exited-session")
        finally:
            other.shutdown();other.server_close();worker.join()

    async def test_streamable_http_authentication_and_same_tools_as_stdio(self):
        editor_process=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"])
        (self.root / "test.json").write_text(json.dumps({**self.descriptor,"pid":editor_process.pid}))
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        token = "test-http-credential-" + "b" * 48
        env = {**os.environ, "VGS_EDITOR_MCP_HTTP_TOKEN": token}
        process = subprocess.Popen([sys.executable, str(SERVER), "--sessions-dir", str(self.root),
                                    "--transport", "streamable-http", "--port", str(port)],
                                   env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        url = f"http://127.0.0.1:{port}/mcp"
        try:
            deadline = time.monotonic() + 12
            while True:
                try:
                    await asyncio.to_thread(urlopen, url, timeout=.5)
                except HTTPError as error:
                    self.assertEqual(error.code, 401)
                    break
                except (URLError, OSError):
                    if process.poll() is not None:
                        self.fail(process.stderr.read().decode())
                    self.assertLess(time.monotonic(), deadline, "HTTP server did not start")
                    await asyncio.sleep(.05)
            with self.assertRaises(HTTPError) as denied:
                await asyncio.to_thread(urlopen, Request(url, headers={"Authorization": "Bearer wrong"}), timeout=2)
            self.assertEqual(denied.exception.code, 401)
            async with Client(self.params) as stdio:
                expected = {tool.name for tool in (await stdio.list_tools()).tools}
            for mode in ("auto", "legacy"):
                async with httpx2.AsyncClient(headers={"Authorization": "Bearer " + token}, trust_env=False) as http:
                    async with Client(streamable_http_client(url, http_client=http), mode=mode) as client:
                        self.assertEqual({tool.name for tool in (await client.list_tools()).tools}, expected)
                        result = await client.call_tool("get_editor_state")
                        self.assertFalse(result.is_error)
                        self.assertEqual(result.structured_content["session_id"], "test-session")
                        image = await client.call_tool("get_viewport_image")
                        self.assertFalse(image.is_error)
                        self.assertEqual(image.content[1].type, "image")
                        changed = await client.call_tool("select_editor_session", {"session_id": "another-editor"})
                        self.assertTrue(changed.is_error)
            # A new editor process/session must be selectable without restarting HTTP.
            editor_process.terminate();editor_process.wait(timeout=5)
            self.bridge.state["session_id"]="restarted-session"
            (self.root / "test.json").write_text(json.dumps({**self.descriptor,"session_id":"restarted-session"}))
            async with httpx2.AsyncClient(headers={"Authorization":"Bearer "+token},trust_env=False) as http:
                async with Client(streamable_http_client(url,http_client=http)) as client:
                    recovered=await client.call_tool("get_editor_state")
                    self.assertFalse(recovered.is_error)
                    self.assertEqual(recovered.structured_content["session_id"],"restarted-session")
                    selected=await client.call_tool("select_editor_session",{"session_id":"restarted-session"})
                    self.assertFalse(selected.is_error)
                    state=await client.call_tool("get_editor_state")
                    self.assertEqual(state.structured_content["session_id"],"restarted-session")
                    reference=await client.read_resource("vgs-editor://tools")
                    self.assertIn("purge-isolated",reference.contents[0].text)
                    self.assertIn("Pointcloud",client.instructions)
        finally:
            if editor_process.poll() is None:
                editor_process.terminate();editor_process.wait(timeout=5)
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            process.stdout.close()
            process.stderr.close()

    async def test_remote_binding_requires_https_and_a_strong_token(self):
        with self.assertRaises(ValueError):
            http_transport.build_http_app(adapter.mcp, "weak-token")
        with self.assertRaises(ValueError):
            http_transport.transport_security("0.0.0.0")
        with self.assertRaises(ValueError):
            http_transport.transport_security("0.0.0.0", "http://example.com/mcp")
        settings = http_transport.transport_security("127.0.0.1", "https://example.com/mcp")
        self.assertTrue(settings.enable_dns_rebinding_protection)
        self.assertIn("example.com", settings.allowed_hosts)

    async def test_connection_profiles_are_per_installation_and_keep_the_token(self):
        root = self.root / "installation"
        data = self.root / "user-data"
        root.mkdir()
        output = data / "connections"
        paths = connections.generate(root, Path(sys.executable), output, data)
        token = (data / "http-token").read_text().strip()
        self.assertGreaterEqual(len(token), 32)
        desktop = json.loads((output / "claude-desktop.json").read_text())
        self.assertEqual(desktop["mcpServers"]["vgs-editor"]["args"], [str(root.resolve() / "server.py")])
        self.assertNotIn(token, (output / "stdio.json").read_text())
        self.assertEqual(json.loads((output / "http.json").read_text())["mcpServers"]["vgs-editor"]["headers"]["Authorization"], "Bearer " + token)
        self.assertEqual(json.loads((output / "claude-code-http.json").read_text()), json.loads((output / "http.json").read_text()))
        vscode = json.loads((output / "vscode.json").read_text())["servers"]["vgs-editor"]
        self.assertEqual((vscode["type"], vscode["args"]), ("stdio", [str(root.resolve() / "server.py")]))
        self.assertEqual(json.loads((output / "vscode-http.json").read_text())["servers"]["vgs-editor"]["headers"]["Authorization"], "Bearer " + token)
        connections.generate(root, Path(sys.executable), output, data)
        self.assertEqual((data / "http-token").read_text().strip(), token)
        self.assertEqual(len(paths), 9)


if __name__ == "__main__":
    unittest.main()
