#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""End-to-end check of the preview server's sound-pack upload.

Runs the mock server (web_preview/mock_server.py) on an ephemeral port, uploads
an AURA Sound Pack (.asp) and verifies that it loaded:

  * POST /api/audio/pack      -> { ok, files, bytes, encoding }
  * GET  /api/audio/library   -> the same number of files, per-file volumes
  * GET  /sound-files/<n>.wav -> a browser-playable PCM16 WAV (ADPCM decoded)
  * POST /api/audio/pack (bad)-> { ok: false, error }

Usage:
    python web_preview/test_mock_pack.py                 # builds a pack from 3 tones
    python web_preview/test_mock_pack.py path\\to\\pack.asp
"""
import importlib.util
import json
import math
import os
import struct
import sys
import tempfile
import threading
import time
import urllib.request
import wave
from http.server import ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))


def _load_mock():
    spec = importlib.util.spec_from_file_location("mock_server", os.path.join(HERE, "mock_server.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _make_tone(path, freq, frames=8000):
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(22050)
    w.writeframes(b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * freq * t / 22050)))
                           for t in range(frames)))
    w.close()


def _build_pack(tmp, adpcm=True):
    srcdir = os.path.join(tmp, "src")
    os.makedirs(srcdir, exist_ok=True)
    for name, freq in (("tone_a", 300), ("tone_b", 700), ("curve-squeal-speed3-loop1.2", 1100)):
        _make_tone(os.path.join(srcdir, name + ".wav"), freq)
    out = os.path.join(tmp, "pack.asp")
    sys.path.insert(0, os.path.join(HERE, "..", "firmware", "tools"))
    import pack_sounds
    pack_sounds.pack(srcdir, out, 22050, False, 100, "name", adpcm)
    return out


def _http(port, method, path, data=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method)
    with urllib.request.urlopen(req, timeout=10) as resp:
        return resp.status, resp.read()


def main():
    mock = _load_mock()
    pack = sys.argv[1] if len(sys.argv) > 1 else None
    tmp = tempfile.mkdtemp(prefix="aura_preview_")
    if not pack:
        pack = _build_pack(tmp)
    with open(pack, "rb") as fh:
        body = fh.read()

    server = ThreadingHTTPServer(("127.0.0.1", 0), mock.Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        status, raw = _http(port, "POST", "/api/audio/pack", body)
        res = json.loads(raw)
        assert status == 200 and res.get("ok"), res
        assert res["files"] >= 1 and res["bytes"] == len(body), res
        print("upload: ok files=%s bytes=%s encoding=%s" %
              (res["files"], res["bytes"], res.get("encoding")))

        status, raw = _http(port, "GET", "/api/audio/library")
        lib = json.loads(raw)
        names = [f["name"] for f in lib["files"]]
        assert lib.get("ok") and len(names) == res["files"], lib
        assert all(0 <= f["vol"] <= 100 for f in lib["files"]), lib
        assert all(f.get("size", 0) > 0 for f in lib["files"]), lib  # file sizes present
        print("library: %d -> %s (sizes %s B)" %
              (len(names), names[:3], [f["size"] for f in lib["files"][:3]]))

        status, raw = _http(port, "GET", "/sound-files/%s.wav" % names[0])
        assert status == 200 and raw[0:4] == b"RIFF" and raw[8:12] == b"WAVE" and len(raw) > 44, \
            (status, len(raw))
        data_len = struct.unpack_from("<I", raw, 40)[0]
        assert data_len == len(raw) - 44, (data_len, len(raw))
        print("sound-files/%s.wav: PCM16 %d bytes, browser-playable" % (names[0], len(raw)))

        # A name with a dot (e.g. loop1.2) must not lose its suffix when served.
        dotted = [n for n in names if "." in n]
        for n in dotted:
            status, raw = _http(port, "GET", "/sound-files/%s" % n)
            assert status == 200 and raw[0:4] == b"RIFF", (n, status, len(raw))
        if dotted:
            print("dotted names playable: %s" % dotted)

        # Live mode consumes these masks: F-key levels and physical outputs.
        _http(port, "POST", "/api/motor?forward=1")
        _http(port, "POST", "/api/function?fn=0&state=1")
        status, raw = _http(port, "GET", "/api/sound/graph/state")
        live = json.loads(raw)
        assert live.get("ok") and (live.get("functions", 0) & 1), live
        assert live.get("outputs", 0) & 1, live  # F0F
        _http(port, "POST", "/api/motor?forward=0")
        status, raw = _http(port, "GET", "/api/sound/graph/state")
        live = json.loads(raw)
        assert live.get("outputs", 0) & 2, live  # F0R
        _http(port, "POST", "/api/function?fn=3&state=1")
        status, raw = _http(port, "GET", "/api/sound/graph/state")
        live = json.loads(raw)
        assert live.get("functions", 0) & (1 << 3), live
        assert live.get("outputs", 0) & (1 << 2), live  # AUX1
        print("graph/state Live masks: functions=0x%x outputs=0x%x" %
              (live["functions"], live["outputs"]))

        status, raw = _http(port, "GET", "/api/sound/graph/state")
        before = json.loads(raw)
        _http(port, "POST", "/api/motor?speed=30&forward=1")
        _http(port, "POST", "/api/function?fn=2&state=1")
        time.sleep(0.85)
        status, raw = _http(port, "GET", "/api/sound/graph/state")
        running = json.loads(raw)
        assert len(running.get("states", [])) == 33, running
        assert running["active"] is True, running
        assert running["states"] != before["states"], (before["states"], running["states"])
        status, raw = _http(port, "GET", "/api/sound/graph/project")
        project = json.loads(raw)["project"]
        engine_id = project["states"][running["states"][0]]["id"]
        assert "idle" not in engine_id.lower() and "off" not in engine_id.lower(), engine_id
        print("graph runner: engine=%s effects changed=%s" %
              (engine_id, running["states"] != before["states"]))

        status, raw = _http(port, "POST", "/api/audio/pack", b"not a sound pack")
        bad = json.loads(raw)
        assert status == 200 and bad.get("ok") is False, bad
        print("invalid pack rejected: %s" % bad.get("error"))
    finally:
        server.shutdown()

    print("PASS: sound pack loaded and verified (%s)" % os.path.basename(pack))
    return 0


if __name__ == "__main__":
    sys.exit(main())
