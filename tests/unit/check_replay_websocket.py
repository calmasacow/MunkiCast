#!/usr/bin/env python3
# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run by libertas-ui --check --check-replay; uses only Python's standard library."""
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time


class Client:
    def __init__(self, path="/"):
        self.socket = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20)
        key = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall((f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{sys.argv[1]}\r\n"
                             f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
                             f"Sec-WebSocket-Protocol: obswebsocket.json\r\n"
                             f"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n\r\n").encode())
        data = b""
        while b"\r\n\r\n" not in data:
            data += self.socket.recv(4096)
        header, self.buffer = data.split(b"\r\n\r\n", 1)
        assert header.startswith(b"HTTP/1.1 101"), header
        assert b"sec-websocket-protocol: obswebsocket.json" in header.lower(), header
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        assert accept.lower() in header.lower()
        self.hello = self.receive()["d"]
        self.events = []

    def read(self, count):
        while len(self.buffer) < count:
            data = self.socket.recv(65536)
            assert data, "Connection closed unexpectedly"
            self.buffer += data
        data, self.buffer = self.buffer[:count], self.buffer[count:]
        return data

    def send(self, value):
        payload = json.dumps(value).encode()
        mask = os.urandom(4)
        header = bytes([0x81, 0x80 | len(payload)]) if len(payload) < 126 else b"\x81\xfe" + struct.pack("!H", len(payload))
        self.socket.sendall(header + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(payload)))

    def receive(self):
        while True:
            opcode, length = self.read(2)
            length &= 127
            if length == 126:
                length = struct.unpack("!H", self.read(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self.read(8))[0]
            payload = self.read(length)
            if opcode & 15 == 8:
                return {"closed": True}
            if opcode & 15 == 1:
                return json.loads(payload)

    def identify(self, password):
        auth = self.hello["authentication"]
        digest = lambda data: base64.b64encode(hashlib.sha256(data).digest())
        proof = digest(digest((password + auth["salt"]).encode()) + auth["challenge"].encode()).decode()
        self.send({"op": 1, "d": {"rpcVersion": 1, "authentication": proof, "eventSubscriptions": 2047}})
        return self.receive()

    def request(self, kind, data=None, code=100):
        self.send({"op": 6, "d": {"requestId": kind, "requestType": kind, "requestData": data or {}}})
        while True:
            message = self.receive()
            if message.get("op") == 5:
                self.events.append(message["d"])
            if message.get("op") == 7:
                assert message["d"]["requestId"] == kind, message
                assert message["d"]["requestStatus"]["code"] == code, message
                assert message["d"]["requestStatus"]["result"] == (code == 100), message
                return message["d"]["responseData"]


if os.environ.get("LIBERTAS_TEST_OBS"):
    client = Client()
    assert client.identify(os.environ["LIBERTAS_WEBSOCKET_PASSWORD"])["op"] == 2
    version = client.request("GetVersion")
    assert version["obsVersion"] == "32.0.0"
    assert "LibertasGetState" not in version["availableRequests"]
    client.request("LibertasGetState", code=204)
    for request in ["GetStudioModeEnabled", "GetProfileList", "GetSceneCollectionList",
                    "GetInputList", "GetOutputList", "GetMonitorList", "GetStats",
                    "GetVideoSettings", "GetRecordStatus", "GetRecordDirectory",
                    "GetReplayBufferStatus", "GetSceneList", "GetGroupList",
                    "GetSceneTransitionList", "GetCurrentSceneTransition", "GetHotkeyList",
                    "GetVirtualCamStatus"]:
        client.request(request)
    assert client.request("GetPersistentData", {"realm": "OBS_WEBSOCKET_DATA_REALM_GLOBAL", "slotName": "protocol-check"})["slotValue"] == {"nested": [1, True, None]}
    native = client.request("CallVendorRequest", {"vendorName": "libertas-studio", "requestType": "LibertasGetState", "requestData": {}})
    assert isinstance(native["responseData"]["streaming"], bool)
    assert "browser_source" in client.request("GetInputKindList")["inputKinds"]
    defaults = client.request("GetInputDefaultSettings", {"inputKind": "ffmpeg_source"})["defaultInputSettings"]
    assert "local_file" in defaults and "looping" in defaults
    scene = client.request("GetCurrentProgramScene")
    added = client.request("CreateInput", {"sceneUuid": scene["sceneUuid"], "inputName": "OBS compatibility color", "inputKind": "color_source_v3", "inputSettings": {"color": 4294901760}})
    identity = {"inputUuid": added["inputUuid"]}
    settings = client.request("GetInputSettings", identity)
    assert settings["inputKind"] == "color_source_v3"
    assert settings["inputSettings"]["color"] == 4294901760
    client.request("RemoveInput", identity)
    client.socket.close()
    print("OBS mode identity, Companion bootstrap, native isolation, vendor tunnel and source kind/settings translation passed.")
    sys.exit(0)

listener = f"00000000:{int(sys.argv[1]):04X}"
assert any(row.split()[1] == listener and row.split()[3] == "0A"
           for row in Path("/proc/net/tcp").read_text().splitlines()[1:]), "Server must bind to 0.0.0.0"
bad = Client()
assert bad.identify("wrong-password").get("closed")
bad.socket.close()
client = Client()
assert client.identify(os.environ["LIBERTAS_WEBSOCKET_PASSWORD"])["op"] == 2
version = client.request("GetVersion")
assert "SaveReplayBuffer" in version["availableRequests"]
assert isinstance(version["supportedImageFormats"], list) and version["platform"]
# Native discovery, complete document access, typed invocation, and command validation.
capabilities = client.request("LibertasGetCapabilities")
assert capabilities["mode"] == "libertas"
assert len(version["availableRequests"]) >= 100
assert client.request("LibertasGetProject")["project"]["sceneDocuments"]
assert len(client.request("LibertasGetCommandTemplates")["templates"]) > 40
assert client.request("GetHotkeyList")["hotkeys"]
methods = client.request("LibertasGetMethods")["methods"]
assert any(m["method"] == "undo()" for m in methods)
original_name = client.request("LibertasGetState")["projectName"]
client.request("LibertasExecuteCommand", {"command": {"cmd": "project.set-name", "name": "Remote test"}})
assert client.request("LibertasGetState")["projectName"] == "Remote test"
client.request("LibertasInvoke", {"method": "undo()", "arguments": []})
assert client.request("LibertasGetState")["projectName"] == original_name
client.request("LibertasExecuteCommand", {"command": {"cmd": "invalid.command"}}, 402)
client.request("LibertasInvoke", {"method": "deleteLater()", "arguments": []}, 600)
client.request("LibertasInvoke", {"method": "addScene(QString)", "arguments": [42]}, 401)
client.request("StartVirtualCam", code=204)
client.request("StartAnythingRecord", code=204)
assert not client.request("GetRecordStatus")["outputActive"]
client.request("SetStudioModeEnabled", {"studioModeEnabled": "yes"}, 401)
client.request("SetSceneItemEnabled", {}, 300)

# Global persistent data survives mode/listener restarts; profile data is undoable.
for realm in ["OBS_WEBSOCKET_DATA_REALM_GLOBAL", "OBS_WEBSOCKET_DATA_REALM_PROFILE"]:
    slot = {"realm": realm, "slotName": "protocol-check"}
    client.request("SetPersistentData", {**slot, "slotValue": {"nested": [1, True, None]}})
    assert client.request("GetPersistentData", slot)["slotValue"] == {"nested": [1, True, None]}
    if realm.endswith("PROFILE"):
        client.request("LibertasInvoke", {"method": "undo()", "arguments": []})
        assert client.request("GetPersistentData", slot)["slotValue"] is None

# OBS scenes, placements, source settings, audio, filters, ordering, and transitions.
original = client.request("GetCurrentProgramScene")
created = client.request("CreateScene", {"sceneName": "Remote protocol check"})
identity = {"sceneUuid": created["sceneUuid"]}
client.request("SetCurrentProgramScene", identity)
assert client.request("GetCurrentProgramScene")["sceneUuid"] == created["sceneUuid"]
client.request("SetStudioModeEnabled", {"studioModeEnabled": True})
client.request("SetCurrentPreviewScene", {"sceneUuid": original["sceneUuid"]})
# Program setters must bypass Studio Mode's UI preview behavior.
client.request("SetCurrentProgramScene", {"sceneUuid": original["sceneUuid"]})
assert client.request("GetCurrentProgramScene")["sceneUuid"] == original["sceneUuid"]
client.request("SetCurrentProgramScene", identity)
client.request("TriggerStudioModeTransition")
assert client.request("GetCurrentProgramScene")["sceneUuid"] == original["sceneUuid"]
client.request("SetStudioModeEnabled", {"studioModeEnabled": False})
client.request("SetCurrentSceneTransition", {"transitionName": "Fade"})
client.request("SetCurrentSceneTransitionDuration", {"transitionDuration": 250})
assert client.request("GetCurrentSceneTransition")["transitionDuration"] == 250
client.request("SetCurrentSceneTransition", {"transitionName": "Cut"})
assert "source.audio-input" in client.request("GetInputKindList")["inputKinds"]
added = client.request("CreateInput", {**identity, "inputName": "Remote audio", "inputKind": "source.audio-input"})
input_id = {"inputUuid": added["inputUuid"]}
item_id = {**identity, "sceneItemId": added["sceneItemId"]}
assert client.request("GetSceneItemId", {**identity, "sourceUuid": added["inputUuid"]})["sceneItemId"] == added["sceneItemId"]
client.request("SetSceneItemEnabled", {**item_id, "sceneItemEnabled": False})
assert not client.request("GetSceneItemEnabled", item_id)["sceneItemEnabled"]
client.request("SetSceneItemLocked", {**item_id, "sceneItemLocked": True})
client.request("SetSceneItemTransform", {**item_id, "sceneItemTransform": {"positionX": 20}}, 604)
client.request("SetSceneItemLocked", {**item_id, "sceneItemLocked": False})
client.request("SetSceneItemTransform", {**item_id, "sceneItemTransform": {"positionX": 20, "positionY": 30}})
assert client.request("GetSceneItemTransform", item_id)["sceneItemTransform"]["positionX"] == 20
copy = client.request("DuplicateSceneItem", item_id)
assert copy["sceneItemId"] != added["sceneItemId"]
client.request("SetSceneItemIndex", {**item_id, "sceneItemIndex": 0})
assert client.request("GetSceneItemIndex", item_id)["sceneItemIndex"] == 0
client.request("SetInputMute", {**input_id, "inputMuted": True})
assert client.request("GetInputMute", input_id)["inputMuted"]
assert not client.request("ToggleInputMute", input_id)["inputMuted"]
client.request("SetInputVolume", {**input_id, "inputVolumeDb": -12})
assert client.request("GetInputVolume", input_id)["inputVolumeDb"] == -12
client.request("SetInputVolume", {**input_id, "inputVolumeMul": -1}, 402)
source_id = {"sourceUuid": added["inputUuid"]}
filter_id = {**source_id, "filterName": "Remote gain"}
client.request("CreateSourceFilter", {**filter_id, "filterKind": "filter.gain", "filterSettings": {"db": -6}})
client.request("SetSourceFilterEnabled", {**filter_id, "filterEnabled": False})
assert not client.request("GetSourceFilter", filter_id)["filterEnabled"]
client.request("SetSourceFilterSettings", {**filter_id, "filterSettings": {"db": -3}})
assert client.request("GetSourceFilter", filter_id)["filterSettings"]["db"] == -3
client.request("RemoveSourceFilter", filter_id)
assert not client.request("GetSourceFilterList", source_id)["filters"]
client.request("RemoveInput", input_id)
assert not client.request("GetSceneItemList", identity)["sceneItems"]
client.request("RemoveScene", identity)
assert any(e["eventType"] == "SceneCreated" for e in client.events)
assert any(e["eventType"] == "InputMuteStateChanged" for e in client.events)

# Batch correlation, ordered execution, halt-on-failure, asynchronous Sleep.
client.send({"op": 8, "d": {"requestId": "batch", "haltOnFailure": True, "requests": [
    {"requestType": "Sleep", "requestData": {"sleepMillis": 1}},
    {"requestType": "GetVersion"}, {"requestType": "NotARequest"}, {"requestType": "GetStats"}]}})
while True:
    response = client.receive()
    if response.get("op") == 9:
        assert response["d"]["requestId"] == "batch"
        assert len(response["d"]["results"]) == 3
        assert response["d"]["results"][0]["requestStatus"]["result"]
        assert not response["d"]["results"][2]["requestStatus"]["result"]
        break
client.send({"op": 3, "d": {"eventSubscriptions": 64}})

# Exercise the official OBS JS transport and the MCP SDK if integration dependencies exist.
repo = Path(__file__).resolve().parents[2]
if (repo / "integrations/mcp/node_modules").exists():
    result = subprocess.run(["node", str(repo / "integrations/mcp/integration.js"), sys.argv[1]],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=25)
    print(result.stdout.decode(), end="")
    assert result.returncode == 0

# Recording is acknowledged by the renderer, then finalized as a playable file.
assert not client.request("GetRecordStatus")["outputActive"]
client.request("StartRecord")
assert client.request("GetRecordStatus")["outputActive"]
time.sleep(0.4)
recording = Path(client.request("StopRecord")["outputPath"])
assert recording.is_file() and recording.stat().st_size > 0
record_probe = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_streams", "-of", "json", str(recording)]))
assert any(s["codec_type"] == "video" for s in record_probe["streams"])
assert not client.request("GetRecordStatus")["outputActive"]
deck_id = client.request("LibertasGetState")["mainDeck"]["id"]
assert client.request("LibertasInvoke", {"method": "saveStinger(QString,QString,QString,int,int,QString)",
    "arguments": [deck_id, "Remote stinger", str(recording), 100, 400, ""]})["result"]
client.request("SetCurrentSceneTransitionSettings", {"transitionSettings": {"transition_point": 120, "duration": 450}})
assert client.request("GetCurrentSceneTransition")["transitionSettings"]["transition_point"] == 120
client.request("SetCurrentSceneTransitionSettings", {"transitionSettings": {"transition_point": 1000}}, 402)
client.request("SetCurrentSceneTransition", {"transitionName": "Cut"})
stinger = next(i for i in client.request("GetInputList")["inputs"] if i["inputName"] == "Remote stinger")
client.request("RemoveInput", {"inputUuid": stinger["inputUuid"]})
# Key sequences dispatch existing bindings without injecting operating-system key events.
client.request("LibertasExecuteCommand", {"command": {"cmd": "project.set-name", "name": "Hotkey check"}})
client.request("TriggerHotkeyByKeySequence", {"keyId": "OBS_KEY_Z", "keyModifiers": {"control": True}})
assert client.request("LibertasGetState")["projectName"] == original_name
screenshot = client.request("LibertasGetScreenshot", {"imageWidth": 128})
assert base64.b64decode(screenshot["imageData"]).startswith(b"\x89PNG\r\n\x1a\n")
assert screenshot["width"] == 128

assert not client.request("GetReplayBufferStatus")["outputActive"]
client.request("StartReplayBuffer")
assert client.request("GetReplayBufferStatus")["outputActive"]
time.sleep(3)
client.request("SaveReplayBuffer")
path = Path(client.request("GetLastReplayBufferReplay")["savedReplayPath"])
assert path.name.endswith("_clip.mkv") and path.stat().st_size > 0
assert path.parent == Path(sys.argv[2])
assert any(e["eventType"] == "ReplayBufferSaved" and e["eventData"]["savedReplayPath"] == str(path) for e in client.events)
probe = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)]))
assert any(s["codec_type"] == "video" for s in probe["streams"])
assert 0.5 < float(probe["format"]["duration"]) < 4
client.request("StopReplayBuffer")
assert not client.request("GetReplayBufferStatus")["outputActive"]
client.socket.close()
print("Replay websocket authentication, start, save, playable clip, and stop passed.")
