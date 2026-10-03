#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Local preview server for the AURA-X decoder web UI.

Serves the real firmware page (firmware/web_ui.html) and answers every
/api/* endpoint with in-memory mock data, so the interface can be developed
and tested in a browser exactly like it renders on the decoder -- without a
board. State is kept in process, so the controls, settings, log and sound
scheme panels behave as on the device.

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

TTY_TABLES = [
    "Старт", "Холостой", "Разгон", "Тяга 1", "Тяга 2", "Тяга 3", "Тяга 4",
    "Тяга 5", "Тяга 6", "Тяга 7", "Тяга 8", "Выбег", "Тормоз", "Останов",
    "Свисток", "Тифон", "Компрессор", "Вентилятор", "Сцепка", "Песок",
]
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


def _new_scheme():
    tables = []
    for i in range(31):
        used = 1 <= i <= len(TTY_TABLES)
        name = TTY_TABLES[i - 1] if used else ""
        tables.append({
            "i": i, "used": 1 if used else 0, "name": name,
            "min": (i - 1) * 6 if used else 0, "max": i * 6 if used else 0,
            "rate": 8 if used else 0, "min_plays": 0 if used else 0,
            "max_plays": 3 if used else 0, "end": 0, "nacc": 0, "ndec": 0,
            "init": ("audio/t%02d_init.wav" % i) if used else "",
            "loop": ("audio/t%02d_loop.wav" % i) if used else "",
            "endf": "",
        })
    extras = [
        {"i": 0, "table": 17, "fn": 3, "dir": 0, "state": 0, "mode": 0, "vol": 100, "rmin": 400, "rmax": 1200},
        {"i": 1, "table": 15, "fn": 5, "dir": 0, "state": 0, "mode": 1, "vol": 80, "rmin": 300, "rmax": 900},
        {"i": 2, "table": 16, "fn": 6, "dir": 0, "state": 0, "mode": 0, "vol": 60, "rmin": 800, "rmax": 1600},
        {"i": 3, "table": 18, "fn": 0, "dir": 0, "state": 0, "mode": 0, "vol": 0, "rmin": 0, "rmax": 0},
    ]
    return {
        "type": 1, "name": "Тепловоз ТЭП70", "start_fn": 8, "sync": 1,
        "flags": 0, "start": 1, "stop": 2, "shutdown": 14,
        "cyl_min": 0, "cyl_max": 8, "cyl_inc": 1,
        "drive": [3, 4, 5, 6, 7], "accel": [2, 3, 4, 5, 6],
        "tables": tables, "extras": extras,
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
        "scheme": _new_scheme(),
        "projects": [{"name": "Тепловоз ТЭП70", "active": True}],
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


def sound_state_json():
    s = STATE["scheme"]
    return ok(type=s["type"], name=s["name"], enabled=True, engine=False,
              table=s["start"], phase=0, speed=0, forward=True)


def sound_scheme_json():
    return ok(**STATE["scheme"])


def sound_projects_json():
    projects = STATE["projects"]
    active = ""
    for p in projects:
        if p["active"]:
            active = p["name"]
    return ok(active=active, count=len(projects),
              projects=[dict(p) for p in projects])


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

    if path == "/api/sound/state":
        return 200, sound_state_json()

    if path == "/api/sound/scheme":
        if method == "POST":
            if q1(query, "type") is not None:
                s["scheme"]["type"] = as_int(q1(query, "type"), s["scheme"]["type"])
            if q1(query, "start_fn") is not None:
                s["scheme"]["start_fn"] = as_int(q1(query, "start_fn"), s["scheme"]["start_fn"])
            if q1(query, "sync") is not None:
                s["scheme"]["sync"] = 1 if as_bool(q1(query, "sync"), False) else 0
            add_log("Звук", "схема сохранена")
            return 200, ok()
        return 200, sound_scheme_json()

    if path == "/api/sound/table":
        i = as_int(q1(query, "i"), -1)
        for t in s["scheme"]["tables"]:
            if t["i"] == i:
                if q1(query, "used") is not None:
                    t["used"] = 1 if as_bool(q1(query, "used"), True) else 0
                for key in ("name", "init", "loop", "endf"):
                    if q1(query, key) is not None:
                        t[key] = q1(query, key)
                for key in ("min", "max", "rate", "min_plays", "max_plays"):
                    if q1(query, key) is not None:
                        t[key] = as_int(q1(query, key), t[key])
        add_log("Звук", "таблица %d сохранена" % i)
        return 200, ok(i=i)

    if path == "/api/sound/extra":
        i = as_int(q1(query, "i"), -1)
        for e in s["scheme"]["extras"]:
            if e["i"] == i and q1(query, "table") is not None:
                e["table"] = as_int(q1(query, "table"), e["table"])
        add_log("Звук", "доп. звук %d сохранён" % i)
        return 200, ok(i=i)

    if path == "/api/sound/lint":
        return 200, ok(problems=0, report="")

    if path == "/api/sound/projects":
        return 200, sound_projects_json()

    if path == "/api/sound/project":
        name = q1(query, "create") or q1(query, "activate") or q1(query, "delete")
        if q1(query, "delete") is not None:
            s["projects"] = [p for p in s["projects"] if p["name"] != q1(query, "delete")]
            add_log("Звук", "схема удалена: %s" % q1(query, "delete"))
        elif q1(query, "create") is not None:
            s["projects"].append({"name": name, "active": False})
            add_log("Звук", "схема создана: %s" % name)
        elif q1(query, "activate") is not None:
            for p in s["projects"]:
                p["active"] = (p["name"] == name)
            s["scheme"]["name"] = name
            add_log("Звук", "активна: %s" % name)
        return 200, ok()

    if path == "/api/sound/upload":
        name = q1(query, "name", "Схема")
        if not any(p["name"] == name for p in s["projects"]):
            s["projects"].append({"name": name, "active": False})
        add_log("Звук", "схема загружена %s" % name)
        return 200, ok()

    if path == "/api/sound/download":
        name = q1(query, "name", "scheme")
        data = ("MDS1 " + name).encode("utf-8")
        return 200, data

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
