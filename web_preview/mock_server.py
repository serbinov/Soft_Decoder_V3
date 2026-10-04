#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Local preview server for the AURA-X decoder web UI.

Serves the real firmware page (firmware/web_ui.html) and answers every
/api/* endpoint with in-memory mock data, so the interface can be developed
and tested in a browser exactly like it renders on the decoder -- without a
board. State is kept in process, so the controls, settings, log and sound
graph editor project behave as on the device.

Run:
    python mock_server.py            # http://127.0.0.1:8080/
    python mock_server.py 9000       # custom port
    set PREVIEW_PORT=9000 & python mock_server.py  (Windows CMD)
"""

import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, unquote

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.normpath(os.path.join(HERE, "..", "firmware"))
UI_FILE = os.path.join(FW, "web_ui.html")
EDITOR_DIR = os.path.join(FW, "sound_editor", "dist")
VERSION = "PREVIEW"
CV_COUNT = 512
FN_COUNT = 29
TRACK_COUNT = 20
AUX_NAMES = ["F0F", "F0R", "AUX1", "AUX2", "AUX3", "AUX4", "AUX5", "AUX6", "AUX7"]

GRAPH_LIMITS = {
    "states": 57, "soundStates": 31, "transitions": 128, "effects": 24,
    "effectTables": 16, "blocks": 64, "sinks": 9, "assets": 31,
    "jsonBytes": 131072,
}
TRACK_LABELS = [
    "Тепловоз", "Отправление", "Свисток", "Тифон", "Компрессор", "Вентилятор",
    "Сцепка", "Тормоз", "Гудок", "Стрелка", "Колёса", "Песок", "Генератор",
    "Дизель", "Токоприёмник", "Шасси", "Дверь", "Кондиционер", "Дребезг",
    "Обрыв",
]

LOCK = threading.Lock()
START = time.time()


def _new_tracks():
    tracks = []
    cats = []
    for i in range(TRACK_COUNT):
        tracks.append({
            "slot": i + 1,
            "file": "audio/slot%02d.wav" % (i + 1),
            "label": TRACK_LABELS[i],
            "enabled": True,
        })
        cats.append(0 if i < 6 else 1)
    return tracks, cats


def _asset(name):
    return {"file": name, "size": 44144, "crc32": "12345678",
            "sampleRate": 22050, "channels": 1, "bits": 16, "durationMs": 1000}


def _state(sid, name, filename, loop, volume=80):
    return {"id": sid, "name": name, "file": filename, "loop": loop,
            "volume": volume, "rate": 1000}


def _edge(eid, source, target, priority, timing, condition):
    return {"id": eid, "source": source, "target": target,
            "priority": priority, "timing": timing, "condition": condition}


def _new_graph():
    """A v2 authoring project (effect-table library + patch-panel routing) with
    a compiled v1 behaviour layer, mirroring what the sound editor saves."""
    states = [
        {"id": "off", "name": "Выкл / тихий вход", "file": "", "loop": False, "volume": 100, "rate": 1000},
        _state("engine_idle", "Холостой ход", "slot01.wav", True),
        _state("engine_run", "Тяга", "slot03.wav", True),
        _state("engine_stop", "Останов двигателя", "slot14.wav", False),
        {"id": "horn_off", "name": "Гудок / тихий вход", "file": "", "loop": False, "volume": 100, "rate": 1000},
        _state("horn_start", "Атака гудка", "slot09.wav", False),
        _state("horn_hold", "Удержание гудка", "slot09.wav", True),
        _state("horn_end", "Отпускание гудка", "slot07.wav", False),
    ]
    transitions = [
        _edge("t1", "off", "engine_idle", 10, "immediate", {"type": "engine_on"}),
        _edge("t2", "engine_idle", "engine_run", 1, "after_sample", {"type": "speed", "min": 1, "max": 255}),
        _edge("t3", "engine_run", "engine_idle", 1, "after_sample", {"type": "speed", "min": 0, "max": 0}),
        _edge("t4", "engine_idle", "engine_stop", 255, "immediate", {"type": "engine_off"}),
        _edge("t5", "engine_run", "engine_stop", 255, "immediate", {"type": "engine_off"}),
        _edge("t6", "engine_stop", "off", 10, "immediate", {"type": "sample_done"}),
        _edge("t7", "horn_off", "horn_start", 10, "immediate", {"type": "fn_press", "fn": 2}),
        _edge("t8", "horn_start", "horn_hold", 10, "immediate", {"type": "sample_done"}),
        _edge("t9", "horn_start", "horn_end", 200, "immediate", {"type": "fn_off", "fn": 2}),
        _edge("t10", "horn_hold", "horn_end", 10, "immediate", {"type": "fn_off", "fn": 2}),
        _edge("t11", "horn_end", "horn_off", 10, "immediate", {"type": "sample_done"}),
    ]
    effect_tables = [
        {"id": "horn", "name": "Гудок", "kind": "horn", "preset": "shortLong",
         "init": "slot09.wav", "loop": "slot09.wav", "end": "slot07.wav", "short": "slot07.wav",
         "shortMs": 400, "volume": 80, "rate": 1000, "behavior": "horn_off"},
        {"id": "motor", "name": "Мотор", "kind": "motor", "preset": "loopHeld",
         "init": "slot01.wav", "loop": "slot01.wav", "end": "slot14.wav", "short": "",
         "shortMs": 400, "volume": 80, "rate": 1000, "behavior": ""},
        {"id": "brake", "name": "Тормоза", "kind": "brake", "preset": "state",
         "init": "", "loop": "", "end": "slot14.wav", "short": "",
         "shortMs": 400, "volume": 70, "rate": 1000, "behavior": ""},
        {"id": "bell", "name": "Звонок", "kind": "bell", "preset": "random",
         "init": "slot09.wav", "loop": "", "end": "", "short": "",
         "shortMs": 400, "volume": 60, "rate": 1000, "behavior": ""},
        {"id": "coupler", "name": "Сцепка", "kind": "coupler", "preset": "oneShot",
         "init": "slot07.wav", "loop": "", "end": "", "short": "",
         "shortMs": 400, "volume": 75, "rate": 1000, "behavior": ""},
    ]
    sources = [
        {"id": "src_engine", "role": "engine", "fn": 8, "label": "Двигатель"},
        {"id": "src_horn", "role": "fn", "fn": 2, "label": "F2 Гудок"},
        {"id": "src_f3", "role": "fn", "fn": 3, "label": "F3 Звонок"},
    ]
    blocks = [
        {"id": "blk_motor", "kind": "sound", "table": "motor", "op": "and", "min": 0, "max": 0},
        {"id": "blk_horn", "kind": "sound", "table": "horn", "op": "and", "min": 0, "max": 0},
        {"id": "blk_bell", "kind": "sound", "table": "bell", "op": "and", "min": 0, "max": 0},
    ]
    sinks = [
        {"id": "sink_head", "output": "F0F"},
        {"id": "sink_aux3", "output": "AUX3"},
    ]
    wires = [
        {"id": "w1", "from": "src_engine", "to": "blk_motor", "event": "fn_on", "dir": "any", "state": "any"},
        {"id": "w2", "from": "src_horn", "to": "blk_horn", "event": "fn_press", "dir": "any", "state": "any"},
        {"id": "w3", "from": "src_f3", "to": "blk_bell", "event": "fn_press", "dir": "any", "state": "any"},
        {"id": "w4", "from": "blk_motor", "to": "sink_head", "event": "", "dir": "fwd", "state": "moving"},
        {"id": "w5", "from": "blk_horn", "to": "sink_aux3", "event": "", "dir": "any", "state": "any"},
    ]
    files = set()
    for st in states:
        if st["file"]:
            files.add(st["file"])
    for tbl in effect_tables:
        for role in ("init", "loop", "end", "short"):
            if tbl[role]:
                files.add(tbl[role])
    positions = {}
    for node in states + sources + blocks + sinks:
        positions[node["id"]] = {"x": 200, "y": 80}
    return {
        "format": "sound-graph", "schemaVersion": 2, "id": "preview",
        "name": "Тепловоз ТЭП70 (превью)",
        "engine": {"entry": "off", "fn": 8}, "hysteresis": 3,
        "effectTables": effect_tables, "sources": sources, "blocks": blocks,
        "sinks": sinks, "wires": wires,
        "states": states, "transitions": transitions,
        "effects": [{"id": "horn", "entry": "horn_off", "fn": 2}],
        "assets": [_asset(name) for name in sorted(files)],
        "editor": {"positions": positions, "viewport": {"x": 0, "y": 0, "zoom": 0.8}},
    }


def _new_state():
    tracks, cats = _new_tracks()
    return {
        "control_source": "web",
        "mode": "dcc",
        "motor": {"speed": 0, "forward": True},
        "fn": [0] * FN_COUNT,
        "audio": {"playing": False, "active_slot": 0, "master": 100, "engine": 100, "effects": 20},
        "device_name": "ТЕПЛОВОЗ",
        "wifi": {
            "mode": 1, "ap_ssid": "AURA-X", "ap_password": "12345678",
            "sta_ssid": "", "sta_password": "", "port": 80, "auto_off_min": 0,
            "hold": 0, "status": "ap_running", "ap_ip": "192.168.100.1",
            "sta_ip": "0.0.0.0",
        },
        "bemf": {"active": False, "progress": 0, "total": 10, "valid": True,
                 "stored": True, "use": False, "runId": 1, "result": "idle",
                 "error": "none", "errorCode": 0},
        "cv": [0] * CV_COUNT,
        "aux": [{"level": 100, "effect": 0} for _ in range(9)],
        "fmap": _new_fmap(),
        "binds": _new_binds(),
        "tracks": tracks,
        "cats": cats,
        "log": [],
        "logseq": 0,
    }


def _new_fmap():
    fmap = []
    for f in range(FN_COUNT):
        entry = {"a": 0, "b": 0, "aux": 0, "dir": 0, "speed": 0}
        if f == 0:
            entry["aux"] = 1
        elif 1 <= f <= TRACK_COUNT:
            entry["a"] = f
        fmap.append(entry)
    return fmap


def _bind(fn, btype, bid, direction, state, mode=0):
    return {
        "fn": fn, "type": btype, "id": bid, "dir": direction, "state": state,
        "mode": mode, "flags": 0, "short": 0, "short_ms": 400,
        "min_ms": 150, "fade_ms": 80,
    }


def _new_binds():
    """Effective bindings mirroring the device defaults after legacy migration:
    F0 head light (bit0 forward, bit1 reverse), F1..F20 -> audio slots, plus a
    couple of sound-table bindings so the "Звук: Функции" matrix shows dots."""
    binds = [
        _bind(0, 1, 0, 1, 0),
        _bind(0, 1, 1, 2, 0),
    ]
    for n in range(1, TRACK_COUNT + 1):
        binds.append(_bind(n, 3, n, 0, 0))
    binds.append(_bind(3, 2, 17, 0, 2, 0))
    binds.append(_bind(5, 2, 15, 0, 2, 1))
    return binds


STATE = _new_state()
STATE["cv"][0] = 3        # CV1 primary address
STATE["cv"][28] = 2       # CV29 28/128 speed steps

# The active sound graph editor project (v2 authoring) and its revision.
GRAPH = {"project": _new_graph(), "revision": 4, "active": False, "fault": False}


def add_log(tag, text):
    STATE["logseq"] += 1
    STATE["log"].append({"s": STATE["logseq"], "tag": tag, "text": text})
    if len(STATE["log"]) > 200:
        STATE["log"] = STATE["log"][-200:]


for _t, _x in [("Система", "предпросмотр запущен"), ("Режим", "DCC")]:
    add_log(_t, _x)


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------

def q1(query, key, default=None):
    vals = query.get(key)
    if not vals:
        return default
    return vals[0]


def as_int(value, default=0):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def as_bool(value, default=False):
    if value is None:
        return default
    return str(value).lower() in ("1", "true", "yes", "on")


def ok(**kw):
    out = {"ok": True}
    out.update(kw)
    return out


def uptime_s():
    return int(time.time() - START)


def device_json():
    return ok(name=STATE["device_name"], version=VERSION, uptime=uptime_s())


def motor_json():
    m = STATE["motor"]
    return ok(speed=m["speed"], forward=m["forward"])


def functions_json():
    return ok(states=list(STATE["fn"]))


def audio_status_json():
    a = STATE["audio"]
    return ok(playing=a["playing"], active_slot=a["active_slot"],
              master_volume=a["master"], engine_volume=a["engine"],
              effects_volume=a["effects"])


def bemf_json():
    b = STATE["bemf"]
    points = [{"speed": i * 12, "frac": min(100, 5 + i * 5)} for i in range(1, 11)]
    return ok(active=b["active"], progress=b["progress"], total=b["total"],
              valid=b["valid"], stored=b["stored"], use=b["use"],
              runId=b["runId"], result=b["result"], error=b["error"],
              errorCode=b["errorCode"], points=points)


def tracks_json():
    return ok(cats=list(STATE["cats"]), tracks=[dict(t) for t in STATE["tracks"] if t["enabled"]])


def fmap_json():
    return ok(map=[dict(m) for m in STATE["fmap"]])


def binds_json():
    return ok(count=len(STATE["binds"]), binds=[dict(b) for b in STATE["binds"]])


def aux_json():
    return ok(aux=[dict(a) for a in STATE["aux"]])


def wifi_json():
    w = STATE["wifi"]
    return ok(mode=w["mode"], ap_ssid=w["ap_ssid"], ap_password=w["ap_password"],
              sta_ssid=w["sta_ssid"], sta_password=w["sta_password"],
              port=w["port"], auto_off_min=w["auto_off_min"], hold=w["hold"],
              status=w["status"], ap_ip=w["ap_ip"], sta_ip=w["sta_ip"])


def log_json(query):
    since = as_int(q1(query, "since", "0"), 0)
    entries = [e for e in STATE["log"] if e["s"] > since]
    return ok(entries=entries, seq=STATE["logseq"])


# --------------------------------------------------------------------------
# API dispatch
# --------------------------------------------------------------------------

def handle_api(method, path, query, body):
    s = STATE

    if path == "/api/device":
        if method == "POST":
            name = q1(query, "name")
            if name:
                s["device_name"] = name
                add_log("Имя", name)
            return 200, ok(name=s["device_name"])
        return 200, device_json()

    if path == "/api/control/source":
        if method == "POST":
            src = (q1(query, "source", "rails") or "rails").lower()
            s["control_source"] = "web" if src in ("web", "1") else "rails"
            add_log("Источник", "Веб" if s["control_source"] == "web" else "Рельсы")
        return 200, ok(source=s["control_source"])

    if path == "/api/mode":
        if method == "POST":
            mode = (q1(query, "mode", "dcc") or "dcc").lower()
            s["mode"] = "dc" if mode == "dc" else "dcc"
            s["motor"]["speed"] = 0
            add_log("Режим", "DC (аналог)" if s["mode"] == "dc" else "DCC")
        return 200, ok(mode=s["mode"])

    if path == "/api/motor":
        if method == "POST":
            if q1(query, "speed") is not None:
                s["motor"]["speed"] = max(0, min(126, as_int(q1(query, "speed"), 0)))
            if q1(query, "forward") is not None:
                s["motor"]["forward"] = as_bool(q1(query, "forward"), True)
        return 200, motor_json()

    if path == "/api/emergency":
        s["motor"]["speed"] = 0
        s["audio"]["playing"] = False
        s["audio"]["active_slot"] = 0
        s["fn"] = [0] * FN_COUNT
        add_log("Стоп", "ЭКСТРЕННЫЙ СТОП")
        return 200, ok()

    if path == "/api/functions":
        return 200, functions_json()

    if path == "/api/function":
        fn = as_int(q1(query, "fn"), -1)
        if 0 <= fn < FN_COUNT:
            s["fn"][fn] = 1 if as_bool(q1(query, "state"), False) else 0
            add_log("Функция", "F%d %s" % (fn, "вкл" if s["fn"][fn] else "выкл"))
            return 200, ok(fn=fn, state=bool(s["fn"][fn]))
        return 200, {"ok": False, "error": "fn out of range"}

    if path == "/api/audio/status":
        return 200, audio_status_json()

    if path == "/api/audio/volume":
        a = s["audio"]
        if q1(query, "master") is not None:
            a["master"] = max(0, min(100, as_int(q1(query, "master"), a["master"])))
        if q1(query, "engine") is not None:
            a["engine"] = max(0, min(100, as_int(q1(query, "engine"), a["engine"])))
        if q1(query, "effects") is not None:
            a["effects"] = max(0, min(100, as_int(q1(query, "effects"), a["effects"])))
        return 200, ok()

    if path == "/api/audio/tracks":
        return 200, tracks_json()

    if path == "/api/audio/play":
        slot = as_int(q1(query, "slot"), 0)
        s["audio"]["playing"] = True
        s["audio"]["active_slot"] = slot
        add_log("Звук", "слот %d" % slot)
        return 200, ok()

    if path == "/api/audio/stop":
        s["audio"]["playing"] = False
        s["audio"]["active_slot"] = 0
        add_log("Звук", "стоп (все)")
        return 200, ok()

    if path == "/api/audio/delete":
        slot = as_int(q1(query, "slot"), 0)
        for t in s["tracks"]:
            if t["slot"] == slot:
                t["enabled"] = False
                t["file"] = ""
                t["label"] = ""
        add_log("Звук", "слот %d удалён" % slot)
        return 200, ok(slot=slot)

    if path == "/api/audio/upload":
        slot = as_int(q1(query, "slot"), 0)
        if slot <= 0:
            used = {t["slot"] for t in s["tracks"] if t["enabled"]}
            slot = next((i for i in range(1, TRACK_COUNT + 1) if i not in used), 0)
        label = "Загружено %d" % slot
        for t in s["tracks"]:
            if t["slot"] == slot:
                t["enabled"] = True
                t["file"] = "audio/slot%02d.wav" % slot
                t["label"] = label
        add_log("Звук", "загружен слот %d (%d байт)" % (slot, len(body)))
        return 200, ok(slot=slot, file="audio/slot%02d.wav" % slot, label=label,
                       enabled=True, bytes=len(body),
                       recovery_cleanup_pending=False, recovery="")

    if path == "/api/track/category":
        slot = as_int(q1(query, "slot"), 0)
        cat = as_int(q1(query, "cat"), 0)
        if 1 <= slot <= TRACK_COUNT:
            s["cats"][slot - 1] = cat
        add_log("Звук", "слот %d категория %d" % (slot, cat))
        return 200, ok(slot=slot, cat=cat)

    if path == "/api/cv/all":
        return 200, ok(count=CV_COUNT, values=list(s["cv"]))

    if path == "/api/cv/write":
        idx = as_int(q1(query, "index"), 0)
        val = max(0, min(255, as_int(q1(query, "value"), 0)))
        if 1 <= idx <= CV_COUNT:
            s["cv"][idx - 1] = val
        add_log("CV", "CV %d = %d" % (idx, val))
        return 200, ok(index=idx, value=val)

    if path == "/api/func-map":
        if q1(query, "view") in ("bind", "matrix"):
            return 200, binds_json()
        if method == "POST":
            if as_bool(q1(query, "bind"), False):
                if as_bool(q1(query, "remove"), False):
                    idx = as_int(q1(query, "idx"), -1)
                    if 0 <= idx < len(s["binds"]):
                        s["binds"].pop(idx)
                    return 200, ok()
                s["binds"].append({
                    "fn": as_int(q1(query, "fn"), 0), "type": as_int(q1(query, "type"), 2),
                    "id": as_int(q1(query, "id"), 0), "dir": as_int(q1(query, "dir"), 0),
                    "state": as_int(q1(query, "state"), 2), "mode": as_int(q1(query, "mode"), 0),
                    "flags": as_int(q1(query, "flags"), 0), "short": as_int(q1(query, "short"), 0),
                    "short_ms": as_int(q1(query, "short_ms"), 400), "min_ms": as_int(q1(query, "min_ms"), 150),
                    "fade_ms": as_int(q1(query, "fade_ms"), 80),
                })
                add_log("Функция", "привязка добавлена")
                return 200, ok(count=len(s["binds"]))
            fn = as_int(q1(query, "fn"), -1)
            if 0 <= fn < FN_COUNT:
                s["fmap"][fn] = {
                    "a": as_int(q1(query, "a"), 0), "b": as_int(q1(query, "b"), 0),
                    "aux": as_int(q1(query, "aux"), 0), "dir": as_int(q1(query, "dir"), 0),
                    "speed": as_int(q1(query, "speed"), 0),
                }
                add_log("Функция", "F%d карта обновлена" % fn)
                return 200, ok(fn=fn)
            return 200, {"ok": False, "error": "save failed"}
        return 200, fmap_json()

    if path == "/api/aux/cfg":
        if method == "POST":
            ch = as_int(q1(query, "ch"), 0)
            if 0 <= ch < len(s["aux"]):
                s["aux"][ch]["level"] = as_int(q1(query, "level"), s["aux"][ch]["level"])
                s["aux"][ch]["effect"] = as_int(q1(query, "effect"), s["aux"][ch]["effect"])
            add_log("AUX", "%s обновлён" % (AUX_NAMES[ch] if 0 <= ch < 9 else ch))
            return 200, ok(ch=ch)
        return 200, aux_json()

    if path == "/api/wifi":
        if method == "POST":
            w = s["wifi"]
            if q1(query, "ap_ssid") is not None:
                w["ap_ssid"] = q1(query, "ap_ssid")
            if q1(query, "ap_ip") is not None:
                w["ap_ip"] = q1(query, "ap_ip")
            add_log("Wi-Fi", "настройки сохранены (IP %s), перезагрузка" % w["ap_ip"])
            return 200, ok(reboot=1)
        return 200, wifi_json()

    if path in ("/api/wifi/reset", "/api/reset"):
        add_log("Сброс", "настройки сброшены, перезагрузка")
        return 200, ok(reboot=1)

    if path == "/api/storage":
        return 200, ok(free=6 * 1024 * 1024)

    if path == "/api/bemf/cal":
        b = s["bemf"]
        if b["active"] and b["progress"] < b["total"]:
            b["progress"] += 1
            if b["progress"] >= b["total"]:
                b["active"] = False
                b["result"] = "succeeded"
                b["stored"] = True
                b["valid"] = True
                add_log("BEMF", "калибровка завершена и сохранена")
        return 200, bemf_json()

    if path == "/api/bemf/calibrate":
        b = s["bemf"]
        if as_bool(q1(query, "cancel"), False):
            b["active"] = False
            b["result"] = "cancelled"
            add_log("BEMF", "калибровка отменена")
        elif as_bool(q1(query, "reset"), False):
            b["active"] = False
            b["stored"] = False
            b["valid"] = False
            b["result"] = "idle"
            add_log("BEMF", "калибровка очищена")
        elif as_bool(q1(query, "start"), False):
            b.update(active=True, progress=0, result="running", error="none",
                     errorCode=0, runId=b["runId"] + 1)
            add_log("BEMF", "калибровка запущена")
        return 200, ok()

    if path == "/api/bemf/use":
        if method == "POST":
            s["bemf"]["use"] = as_bool(q1(query, "enabled"), False)
            add_log("BEMF", "регулятор %s" % ("включён" if s["bemf"]["use"] else "выключен, только ШИМ"))
        return 200, ok(enabled=s["bemf"]["use"])

    if path == "/api/bemf/base":
        return 200, ok(start=20, full=95,
                       points=[{"speed": i * 12, "frac": 20 + i * 7} for i in range(1, 11)])

    if path == "/api/sound/graph/capabilities":
        return 200, ok(format="sound-graph", schemaVersion=1, limits=GRAPH_LIMITS)

    if path == "/api/sound/graph/projects":
        p = GRAPH["project"]
        return 200, ok(projects=[{"id": p["id"], "name": p["name"], "revision": GRAPH["revision"]}])

    if path == "/api/sound/graph/project":
        return 200, ok(project=GRAPH["project"], revision=GRAPH["revision"])

    if path == "/api/sound/graph/state":
        m = s["motor"]
        return 200, ok(active=GRAPH["active"], fault=GRAPH["fault"],
                       id=GRAPH["project"]["id"], revision=GRAPH["revision"],
                       engine=False, armed=True, states=[0],
                       failedChannels=0, speed=m["speed"])

    if path == "/api/sound/graph/asset":
        return 200, ok(asset=_asset(q1(query, "file", "sample.wav")))

    if path == "/api/sound/graph/validate":
        return 200, ok(valid=True, diagnostics=[])

    if path == "/api/sound/graph/save":
        try:
            incoming = json.loads(body.decode("utf-8")) if body else {}
        except (ValueError, UnicodeDecodeError):
            return 200, {"ok": False, "error": "invalid json"}
        if isinstance(incoming, dict):
            # The editor sends the compiled schema-v1 payload; keep the v2
            # authoring layer so preview load/save round-trips the library.
            if not incoming.get("effectTables"):
                for key in ("effectTables", "sources", "blocks", "sinks", "wires"):
                    if key in GRAPH["project"]:
                        incoming[key] = GRAPH["project"][key]
            GRAPH["project"] = incoming
        expected = as_int(q1(query, "expectedRevision"), 0)
        GRAPH["revision"] = expected + 1 if expected else GRAPH["revision"] + 1
        add_log("Граф", "сохранена версия %d" % GRAPH["revision"])
        return 200, ok(revision=GRAPH["revision"])

    if path == "/api/sound/graph/apply":
        GRAPH["active"] = True
        GRAPH["fault"] = False
        add_log("Граф", "применён проект %s" % GRAPH["project"].get("id", ""))
        return 200, ok()

    if path == "/api/ota/update":
        add_log("OTA", "прошивка записана, перезагрузка")
        return 200, ok(bytes=len(body), files=1, recovery_cleanup_pending=False, recovery="")

    if path == "/api/log":
        return 200, log_json(query)

    if path == "/api/clientlog":
        sys.stderr.write("[client] %s\n" % (q1(query, "m", "") or ""))
        return 200, ok()

    if path == "/api/time":
        return 200, ok(now=int(time.time()))

    return 200, {"ok": False, "error": "preview: endpoint not implemented: %s" % path}


# --------------------------------------------------------------------------
# HTTP server
# --------------------------------------------------------------------------

CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".txt": "text/plain; charset=utf-8",
    ".gz": "application/gzip",
    ".svg": "image/svg+xml",
    ".png": "image/png",
}


class Handler(BaseHTTPRequestHandler):
    server_version = "AuraPreview/1.0"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    # -- low level ---------------------------------------------------------
    def _send(self, status, body, ctype="application/json; charset=utf-8", extra=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        if extra:
            for key, value in extra:
                self.send_header(key, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

    def _json(self, status, obj):
        self._send(status, json.dumps(obj, ensure_ascii=False))

    def _serve_file(self, path):
        if not os.path.isfile(path):
            return self._json(404, {"ok": False, "error": "not found"})
        ext = os.path.splitext(path)[1].lower()
        with open(path, "rb") as fh:
            data = fh.read()
        ctype = CONTENT_TYPES.get(ext, "application/octet-stream")
        self._send(200, data, ctype)

    def _serve_ui(self):
        try:
            with open(UI_FILE, "r", encoding="utf-8") as fh:
                html = fh.read()
        except OSError as exc:
            return self._send(500, "web_ui.html not found: %s" % exc, "text/plain; charset=utf-8")
        html = html.replace("__VERSION__", VERSION)
        self._send(200, html, "text/html; charset=utf-8")

    # -- routing -----------------------------------------------------------
    def _route(self, method):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query, keep_blank_values=True)

        if path == "/favicon.ico":
            return self._send(204, b"", "image/x-icon")
        if path in ("/", "/index.html"):
            return self._serve_ui()
        if path in ("/sound-editor", "/sound-editor/"):
            return self._serve_file(os.path.join(EDITOR_DIR, "index.html"))
        if path.startswith("/sound-editor/"):
            rel = unquote(path[len("/sound-editor/"):])
            safe = os.path.normpath(os.path.join(EDITOR_DIR, rel))
            if safe.startswith(EDITOR_DIR):
                return self._serve_file(safe)
            return self._json(404, {"ok": False, "error": "not found"})

        if path.startswith("/api/") or path == "/api":
            length = as_int(self.headers.get("Content-Length"), 0)
            body = self.rfile.read(length) if length > 0 else b""
            try:
                status, payload = handle_api(method, path, query, body)
            except Exception as exc:  # keep the preview alive on bad input
                status, payload = 500, {"ok": False, "error": "preview error: %s" % exc}
            if isinstance(payload, (bytes, bytearray)):
                return self._send(status, bytes(payload), "application/octet-stream")
            return self._json(status, payload)

        return self._json(404, {"ok": False, "error": "not found"})

    def do_GET(self):
        self._route("GET")

    def do_HEAD(self):
        self._route("GET")

    def do_POST(self):
        self._route("POST")

    def do_OPTIONS(self):
        self._send(204, b"", "text/plain; charset=utf-8",
                   extra=[("Access-Control-Allow-Methods", "GET, POST, OPTIONS"),
                          ("Access-Control-Allow-Headers", "*")])


def main():
    port = as_int(os.environ.get("PREVIEW_PORT"), 0)
    if len(sys.argv) > 1:
        port = as_int(sys.argv[1], port)
    if port <= 0:
        port = 8080
    host = os.environ.get("PREVIEW_HOST", "127.0.0.1")
    server = ThreadingHTTPServer((host, port), Handler)
    url = "http://%s:%d/" % (host, port)
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    print("AURA-X preview server")
    print("  UI:     %s" % url)
    print("  source: %s" % UI_FILE)
    print("  stop:   Ctrl+C")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
